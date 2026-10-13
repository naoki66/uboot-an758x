// SPDX-License-Identifier: GPL-2.0+
/*
 * Status LED pattern for the U-Boot HTTP management service.
 *
 * The pattern is a highlight that sweeps back and forth across every status
 * LED the board device tree declares, modulated by a slow breathing envelope.
 * With a single status LED the sweep degenerates into a plain breath, and the
 * SoC flash/LED PWM mux is used for that pin when the hardware allows it.
 *
 * Register level access is required on purpose: the LED uclass owns the same
 * pins (they are declared as gpio-leds) but only offers on/off, while the
 * pattern needs per-LED brightness at ~50 Hz.
 *
 * The sweep/breath maths and the SoC mux handling follow the Brightspeed
 * XR1710G U-Boot port (YYH2913/http-uboot-xr1710g), whose recovery page
 * drives the front status LEDs in an alternating rainbow breathing pattern.
 *
 * Register map notes (Airoha EN7581/AN7581, cross-checked against
 * drivers/pinctrl/airoha/):
 *   - GPIO data / output-enable / direction and the flash-LED PWM mux live in
 *     the GPIO system controller at 0x1fbf0200.
 *   - The LAN LED mux lives in the chip SCU at 0x1fa20000 and can override a
 *     pin that is otherwise a plain GPIO.
 */

#include <asm/io.h>
#include <dm/ofnode.h>
#include <dt-bindings/gpio/gpio.h>
#include <led.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/kconfig.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <time.h>

#ifdef CONFIG_HTTPD_LED_PATTERN

#define AIROHA_NUM_PINS			64
#define AIROHA_PIN_BANK_SIZE		32
#define AIROHA_REG_GPIOCTRL_NUM_PIN	16

#define HTTPD_LED_GPIO_SYSCTL_BASE	0x1fbf0200
#define HTTPD_LED_CHIP_SCU_BASE		0x1fa20000

#define HTTPD_LED_REG_GPIO_DATA		0x0004
#define HTTPD_LED_REG_GPIO_OE		0x0014
#define HTTPD_LED_REG_GPIO_CTRL		0x0000
#define HTTPD_LED_REG_GPIO_CTRL1	0x0020
#define HTTPD_LED_REG_GPIO_CTRL2	0x0060
#define HTTPD_LED_REG_GPIO_CTRL3	0x0064
#define HTTPD_LED_REG_GPIO_DATA1	0x0070
#define HTTPD_LED_REG_GPIO_OE1		0x0078
#define HTTPD_LED_REG_GPIO_FLASH_MODE_CFG	0x0034
#define HTTPD_LED_REG_GPIO_FLASH_MODE_CFG_EXT	0x0068
#define HTTPD_LED_REG_GPIO_FLASH_PRD_SET0	0x003c
#define HTTPD_LED_REG_GPIO_FLASH_MAP0	0x004c
#define HTTPD_LED_REG_GPIO_FLASH_MAP1	0x0050
#define HTTPD_LED_REG_CYCLE_CFG_VALUE0	0x0098

#define HTTPD_LED_REG_GPIO_2ND_I2C_MODE	0x0214

#define HTTPD_LED_FLASH_MAP_BUCKET0	0x88888888
#define HTTPD_LED_FLASH_DUTY_FULL	255

#define HTTPD_LED_STATUS_MAX		8
#define HTTPD_LED_SW_PWM_PERIOD_MS	20
#define HTTPD_LED_HW_PWM_UPDATE_MS	40
#define HTTPD_LED_HW_PWM_PERIOD_TICKS	1
#define HTTPD_LED_BREATHE_PERIOD_MS	1800
#define HTTPD_LED_BREATHE_HALF_MS	(HTTPD_LED_BREATHE_PERIOD_MS / 2)
#define HTTPD_LED_SWEEP_STEP_MS		700
#define HTTPD_LED_OVERLAP_FP		(2 * 256)
#define HTTPD_LED_BRIGHTNESS_FP		256
#define HTTPD_LED_BREATHE_MIN_FP	64

struct httpd_led_gpio_pin {
	ofnode node;
	u8 gpio;
	s8 last_on;
	bool active_low;
	bool valid;
};

struct httpd_led_status_ctrl {
	struct httpd_led_gpio_pin leds[HTTPD_LED_STATUS_MAX];
	int led_count;
	ulong start_ms;
	ulong last_pwm_update;
	u32 pwm_mux_mask;
	u32 pwm_mux_mask_ext;
	u32 saved_pwm_duty;
	u32 saved_pwm_map[2];
	u32 saved_pwm_cycle;
	bool pwm_active_low;
	bool hw_pwm;
	bool active;
};

static struct httpd_led_status_ctrl httpd_led_status;

static void httpd_led_clrsetbits_le32(uintptr_t addr, u32 clear, u32 set)
{
	u32 val = readl((void __iomem *)addr);

	val &= ~clear;
	val |= set;
	writel(val, (void __iomem *)addr);
}

/*
 * A GPIO can be muxed to a flash/LED hardware function. Two disjoint ranges
 * share the "extended" mux register with different bit offsets, which is why
 * the mapping is not a single shift.
 */
static bool httpd_led_flash_mode_bit(u8 gpio, uintptr_t *reg, u32 *mask)
{
	if (gpio <= 15) {
		*reg = HTTPD_LED_GPIO_SYSCTL_BASE +
		       HTTPD_LED_REG_GPIO_FLASH_MODE_CFG;
		*mask = BIT(gpio);
		return true;
	}

	if (gpio >= 16 && gpio <= 31) {
		*reg = HTTPD_LED_GPIO_SYSCTL_BASE +
		       HTTPD_LED_REG_GPIO_FLASH_MODE_CFG_EXT;
		*mask = BIT(gpio - 16);
		return true;
	}

	if (gpio >= 36 && gpio <= 51) {
		*reg = HTTPD_LED_GPIO_SYSCTL_BASE +
		       HTTPD_LED_REG_GPIO_FLASH_MODE_CFG_EXT;
		*mask = BIT(gpio - 20);
		return true;
	}

	return false;
}

/*
 * The LAN LED mux overrides the pin function and lives in a different
 * register block than the flash mux. Gemtek boards reuse LAN LED pins for the
 * status LEDs, so it has to be detached as well.
 */
static bool httpd_led_lan_led_mode_bit(u8 gpio, u32 *mask)
{
	static const u8 lan_led_pins[] = {
		33, 43, 34, 44, 35, 45, 42, 46,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lan_led_pins); i++) {
		if (lan_led_pins[i] != gpio)
			continue;

		*mask = BIT(3 + i);
		return true;
	}

	return false;
}

static uintptr_t httpd_led_gpio_data_reg(u8 gpio)
{
	return HTTPD_LED_GPIO_SYSCTL_BASE +
	       (gpio < AIROHA_PIN_BANK_SIZE ? HTTPD_LED_REG_GPIO_DATA :
		HTTPD_LED_REG_GPIO_DATA1);
}

static uintptr_t httpd_led_gpio_oe_reg(u8 gpio)
{
	return HTTPD_LED_GPIO_SYSCTL_BASE +
	       (gpio < AIROHA_PIN_BANK_SIZE ? HTTPD_LED_REG_GPIO_OE :
		HTTPD_LED_REG_GPIO_OE1);
}

static uintptr_t httpd_led_gpio_dir_reg(u8 gpio)
{
	static const u16 dir_regs[] = {
		HTTPD_LED_REG_GPIO_CTRL,
		HTTPD_LED_REG_GPIO_CTRL1,
		HTTPD_LED_REG_GPIO_CTRL2,
		HTTPD_LED_REG_GPIO_CTRL3,
	};

	return HTTPD_LED_GPIO_SYSCTL_BASE + dir_regs[gpio / AIROHA_REG_GPIOCTRL_NUM_PIN];
}

static void httpd_led_gpio_direction_output(u8 gpio)
{
	u32 bank_bit = BIT(gpio % AIROHA_PIN_BANK_SIZE);
	u32 shift = 2 * (gpio % AIROHA_REG_GPIOCTRL_NUM_PIN);
	u32 dir_mask = GENMASK(shift + 1, shift);

	httpd_led_clrsetbits_le32(httpd_led_gpio_oe_reg(gpio), 0, bank_bit);
	/*
	 * Both mode bits select the pin function (01 = output). Write the full
	 * pair: leaving a stale bit1 set keeps the pin off the GPIO function.
	 * This mirrors airoha_gpio_set_direction() in the pinctrl driver.
	 */
	httpd_led_clrsetbits_le32(httpd_led_gpio_dir_reg(gpio), dir_mask,
				  BIT(shift));
}

static void httpd_led_gpio_prepare_output(u8 gpio)
{
	uintptr_t reg;
	u32 mask;

	/* Detach the pin from the flash/LED mux so it behaves as a GPIO. */
	if (httpd_led_flash_mode_bit(gpio, &reg, &mask))
		httpd_led_clrsetbits_le32(reg, mask, 0);

	if (httpd_led_lan_led_mode_bit(gpio, &mask))
		httpd_led_clrsetbits_le32(HTTPD_LED_CHIP_SCU_BASE +
					  HTTPD_LED_REG_GPIO_2ND_I2C_MODE,
					  mask, 0);

	httpd_led_gpio_direction_output(gpio);
}

static void httpd_led_gpio_set_value(u8 gpio, bool active_low, int active)
{
	u32 bit = BIT(gpio % AIROHA_PIN_BANK_SIZE);
	uintptr_t reg = httpd_led_gpio_data_reg(gpio);
	u32 set = active_low ? (active ? 0 : bit) : (active ? bit : 0);

	httpd_led_clrsetbits_le32(reg, bit, set);
}

static void httpd_led_pin_set(struct httpd_led_gpio_pin *pin, int on)
{
	if (!pin->valid)
		return;

	on = !!on;
	if (pin->last_on == on)
		return;

	httpd_led_gpio_set_value(pin->gpio, pin->active_low, on);
	pin->last_on = on;
}

static void httpd_led_pin_release(struct httpd_led_gpio_pin *pin)
{
	if (!pin->valid)
		return;

	memset(pin, 0, sizeof(*pin));
}

static ofnode httpd_led_alias_node(const char *alias)
{
	ofnode aliases;
	const char *path;

	aliases = ofnode_path("/aliases");
	if (!ofnode_valid(aliases))
		return ofnode_null();

	path = ofnode_read_string(aliases, alias);
	if (!path)
		return ofnode_null();

	return ofnode_path(path);
}

static int httpd_led_node_to_gpio(ofnode node, struct httpd_led_gpio_pin *pin)
{
	struct ofnode_phandle_args args;
	int ret;

	memset(pin, 0, sizeof(*pin));

	if (!ofnode_valid(node))
		return -ENOENT;

	ret = ofnode_parse_phandle_with_args(node, "gpios", "#gpio-cells", 0, 0,
					     &args);
	if (ret)
		return ret;

	if (args.args_count < 1)
		return -EINVAL;

	pin->gpio = args.args[0];
	pin->active_low = args.args_count > 1 &&
			 (args.args[1] & GPIO_ACTIVE_LOW);
	pin->last_on = -1;
	pin->node = node;
	pin->valid = true;

	return 0;
}

static void httpd_led_status_add(struct httpd_led_status_ctrl *ctrl,
				 const struct httpd_led_gpio_pin *pin)
{
	int i;

	if (!pin->valid)
		return;

	/* The SoC exposes AIROHA_NUM_PINS GPIOs; the rest is not drivable. */
	if (pin->gpio >= AIROHA_NUM_PINS)
		return;

	for (i = 0; i < ctrl->led_count; i++) {
		if (ctrl->leds[i].gpio == pin->gpio)
			return;
	}

	if (ctrl->led_count >= ARRAY_SIZE(ctrl->leds))
		return;

	ctrl->leds[ctrl->led_count++] = *pin;
}

static void httpd_led_status_collect_alias(struct httpd_led_status_ctrl *ctrl,
					   const char *alias)
{
	struct httpd_led_gpio_pin pin;

	if (!httpd_led_node_to_gpio(httpd_led_alias_node(alias), &pin))
		httpd_led_status_add(ctrl, &pin);
}

static void httpd_led_status_request(struct httpd_led_gpio_pin *pin)
{
	httpd_led_gpio_prepare_output(pin->gpio);
	httpd_led_gpio_set_value(pin->gpio, pin->active_low, 0);
	pin->last_on = 0;
}

static void httpd_led_hw_pwm_set(struct httpd_led_status_ctrl *ctrl,
				 u32 brightness_fp)
{
	u32 high_ticks, value;

	high_ticks = (brightness_fp * HTTPD_LED_FLASH_DUTY_FULL +
		      HTTPD_LED_BRIGHTNESS_FP / 2) /
		     HTTPD_LED_BRIGHTNESS_FP;
	if (ctrl->pwm_active_low)
		high_ticks = HTTPD_LED_FLASH_DUTY_FULL - high_ticks;

	value = ((HTTPD_LED_FLASH_DUTY_FULL - high_ticks) << 8) | high_ticks;
	httpd_led_clrsetbits_le32(HTTPD_LED_GPIO_SYSCTL_BASE +
				  HTTPD_LED_REG_GPIO_FLASH_PRD_SET0,
				  GENMASK(15, 0), value);
}

static int httpd_led_hw_pwm_init(struct httpd_led_status_ctrl *ctrl)
{
	u32 flash_mode, flash_mode_ext;
	int i;

	ctrl->pwm_active_low = ctrl->leds[0].active_low;
	for (i = 0; i < ctrl->led_count; i++) {
		uintptr_t reg;
		u32 mask;

		/*
		 * The hardware PWM bucket is shared by all muxed pins, so a
		 * mixed polarity set cannot be driven by one duty value.
		 */
		if (ctrl->leds[i].active_low != ctrl->pwm_active_low ||
		    !httpd_led_flash_mode_bit(ctrl->leds[i].gpio, &reg, &mask))
			return -EOPNOTSUPP;

		if (reg == HTTPD_LED_GPIO_SYSCTL_BASE +
			   HTTPD_LED_REG_GPIO_FLASH_MODE_CFG)
			ctrl->pwm_mux_mask |= mask;
		else
			ctrl->pwm_mux_mask_ext |= mask;
	}

	/*
	 * Somebody else is already using the LED/flash mux (NAND, for
	 * instance). Stay out of it and fall back to software PWM.
	 */
	flash_mode = readl((void __iomem *)(HTTPD_LED_GPIO_SYSCTL_BASE +
					    HTTPD_LED_REG_GPIO_FLASH_MODE_CFG));
	flash_mode_ext = readl((void __iomem *)(HTTPD_LED_GPIO_SYSCTL_BASE +
						HTTPD_LED_REG_GPIO_FLASH_MODE_CFG_EXT));
	if ((flash_mode & ~ctrl->pwm_mux_mask) ||
	    (flash_mode_ext & ~ctrl->pwm_mux_mask_ext))
		return -EBUSY;

	ctrl->saved_pwm_duty = readl((void __iomem *)(HTTPD_LED_GPIO_SYSCTL_BASE +
						      HTTPD_LED_REG_GPIO_FLASH_PRD_SET0));
	ctrl->saved_pwm_map[0] = readl((void __iomem *)(HTTPD_LED_GPIO_SYSCTL_BASE +
							HTTPD_LED_REG_GPIO_FLASH_MAP0));
	ctrl->saved_pwm_map[1] = readl((void __iomem *)(HTTPD_LED_GPIO_SYSCTL_BASE +
							HTTPD_LED_REG_GPIO_FLASH_MAP1));
	ctrl->saved_pwm_cycle = readl((void __iomem *)(HTTPD_LED_GPIO_SYSCTL_BASE +
						       HTTPD_LED_REG_CYCLE_CFG_VALUE0));

	httpd_led_clrsetbits_le32(HTTPD_LED_GPIO_SYSCTL_BASE +
				  HTTPD_LED_REG_CYCLE_CFG_VALUE0,
				  GENMASK(7, 0),
				  HTTPD_LED_HW_PWM_PERIOD_TICKS);
	httpd_led_hw_pwm_set(ctrl, 0);
	/* Extended PWM muxes use fixed GPIO0..15 channels; share one bucket. */
	writel(HTTPD_LED_FLASH_MAP_BUCKET0,
	       (void __iomem *)(HTTPD_LED_GPIO_SYSCTL_BASE +
				HTTPD_LED_REG_GPIO_FLASH_MAP0));
	writel(HTTPD_LED_FLASH_MAP_BUCKET0,
	       (void __iomem *)(HTTPD_LED_GPIO_SYSCTL_BASE +
				HTTPD_LED_REG_GPIO_FLASH_MAP1));
	httpd_led_clrsetbits_le32(HTTPD_LED_GPIO_SYSCTL_BASE +
				  HTTPD_LED_REG_GPIO_FLASH_MODE_CFG,
				  0, ctrl->pwm_mux_mask);
	httpd_led_clrsetbits_le32(HTTPD_LED_GPIO_SYSCTL_BASE +
				  HTTPD_LED_REG_GPIO_FLASH_MODE_CFG_EXT,
				  0, ctrl->pwm_mux_mask_ext);

	ctrl->hw_pwm = true;

	return 0;
}

static void httpd_led_hw_pwm_stop(struct httpd_led_status_ctrl *ctrl)
{
	int i;

	httpd_led_hw_pwm_set(ctrl, 0);
	httpd_led_clrsetbits_le32(HTTPD_LED_GPIO_SYSCTL_BASE +
				  HTTPD_LED_REG_GPIO_FLASH_MODE_CFG,
				  ctrl->pwm_mux_mask, 0);
	httpd_led_clrsetbits_le32(HTTPD_LED_GPIO_SYSCTL_BASE +
				  HTTPD_LED_REG_GPIO_FLASH_MODE_CFG_EXT,
				  ctrl->pwm_mux_mask_ext, 0);

	writel(ctrl->saved_pwm_map[0],
	       (void __iomem *)(HTTPD_LED_GPIO_SYSCTL_BASE +
				HTTPD_LED_REG_GPIO_FLASH_MAP0));
	writel(ctrl->saved_pwm_map[1],
	       (void __iomem *)(HTTPD_LED_GPIO_SYSCTL_BASE +
				HTTPD_LED_REG_GPIO_FLASH_MAP1));
	writel(ctrl->saved_pwm_duty,
	       (void __iomem *)(HTTPD_LED_GPIO_SYSCTL_BASE +
				HTTPD_LED_REG_GPIO_FLASH_PRD_SET0));
	writel(ctrl->saved_pwm_cycle,
	       (void __iomem *)(HTTPD_LED_GPIO_SYSCTL_BASE +
				HTTPD_LED_REG_CYCLE_CFG_VALUE0));

	ctrl->hw_pwm = false;
	for (i = 0; i < ctrl->led_count; i++) {
		ctrl->leds[i].last_on = -1;
		httpd_led_pin_set(&ctrl->leds[i], 0);
	}
}

static void httpd_led_status_release(struct httpd_led_status_ctrl *ctrl)
{
	int i;

	for (i = 0; i < ctrl->led_count; i++)
		httpd_led_pin_release(&ctrl->leds[i]);

	memset(ctrl, 0, sizeof(*ctrl));
}

static int httpd_led_status_init(struct httpd_led_status_ctrl *ctrl)
{
	static const char *const status_aliases[] = {
		"led-boot",
		"led-running",
		"led-failsafe",
		"led-upgrade",
	};
	ofnode leds, node;
	int i;

	memset(ctrl, 0, sizeof(*ctrl));

	/* Every LED the board marks as a status indicator takes part. */
	leds = ofnode_path("/leds");
	if (ofnode_valid(leds)) {
		ofnode_for_each_subnode(node, leds) {
			const char *function;
			struct httpd_led_gpio_pin pin;

			function = ofnode_read_string(node, "function");
			if (!function || strcmp(function, "status"))
				continue;

			if (!httpd_led_node_to_gpio(node, &pin))
				httpd_led_status_add(ctrl, &pin);
		}
	}

	for (i = 0; i < ARRAY_SIZE(status_aliases); i++)
		httpd_led_status_collect_alias(ctrl, status_aliases[i]);

	if (!ctrl->led_count)
		return -ENOENT;

	for (i = 0; i < ctrl->led_count; i++)
		httpd_led_status_request(&ctrl->leds[i]);

	ctrl->start_ms = get_timer(0);
	if (ctrl->led_count == 1) {
		int pwm_ret = httpd_led_hw_pwm_init(ctrl);

		if (pwm_ret)
			printf("HTTP status LED: hardware PWM unavailable (%d), using GPIO PWM\n",
			       pwm_ret);
	}

	printf("HTTP status LEDs:");
	for (i = 0; i < ctrl->led_count; i++)
		printf(" gpio%u%s", ctrl->leds[i].gpio,
		       ctrl->leds[i].active_low ? "(L)" : "");
	printf(" (%s PWM)\n", ctrl->hw_pwm ? "hardware" : "software");

	return 0;
}

/* Triangle wave: dim -> bright -> dim over one breathe period. */
static u32 httpd_led_breath_fp(ulong elapsed)
{
	ulong phase, ramp;
	u32 delta;

	phase = elapsed % HTTPD_LED_BREATHE_PERIOD_MS;
	ramp = phase < HTTPD_LED_BREATHE_HALF_MS ?
	       phase : HTTPD_LED_BREATHE_PERIOD_MS - phase;
	delta = HTTPD_LED_BRIGHTNESS_FP - HTTPD_LED_BREATHE_MIN_FP;

	return HTTPD_LED_BREATHE_MIN_FP +
	       (delta * ramp * ramp +
		(HTTPD_LED_BREATHE_HALF_MS *
		 HTTPD_LED_BREATHE_HALF_MS) / 2) /
	       (HTTPD_LED_BREATHE_HALF_MS *
		HTTPD_LED_BREATHE_HALF_MS);
}

/* Position of the sweep highlight, in "LED index" units scaled by BRIGHTNESS_FP. */
static ulong httpd_led_position_fp(struct httpd_led_status_ctrl *ctrl,
				   ulong elapsed)
{
	ulong range_fp, phase;
	int span;

	if (ctrl->led_count <= 1)
		return 0;

	span = ctrl->led_count - 1;
	range_fp = span * HTTPD_LED_BRIGHTNESS_FP;
	phase = elapsed % (2 * span * HTTPD_LED_SWEEP_STEP_MS);

	if (phase <= span * HTTPD_LED_SWEEP_STEP_MS)
		return phase * HTTPD_LED_BRIGHTNESS_FP /
		       HTTPD_LED_SWEEP_STEP_MS;

	phase -= span * HTTPD_LED_SWEEP_STEP_MS;

	return range_fp - (phase * HTTPD_LED_BRIGHTNESS_FP /
			   HTTPD_LED_SWEEP_STEP_MS);
}

static u32 httpd_led_brightness_fp(struct httpd_led_status_ctrl *ctrl,
				   int idx, ulong elapsed)
{
	ulong center_fp, pwm_phase;
	u32 breath_fp, weight_fp, dist_fp;
	ulong led_pos_fp;

	center_fp = httpd_led_position_fp(ctrl, elapsed);
	led_pos_fp = idx * HTTPD_LED_BRIGHTNESS_FP;
	dist_fp = center_fp > led_pos_fp ? center_fp - led_pos_fp :
					   led_pos_fp - center_fp;
	if (dist_fp >= HTTPD_LED_OVERLAP_FP)
		return 0;

	weight_fp = (HTTPD_LED_OVERLAP_FP - dist_fp) *
		    HTTPD_LED_BRIGHTNESS_FP / HTTPD_LED_OVERLAP_FP;
	weight_fp = weight_fp * weight_fp / HTTPD_LED_BRIGHTNESS_FP;
	breath_fp = httpd_led_breath_fp(elapsed);
	pwm_phase = weight_fp * breath_fp;
	pwm_phase = (pwm_phase + HTTPD_LED_BRIGHTNESS_FP / 2) /
		    HTTPD_LED_BRIGHTNESS_FP;

	return pwm_phase;
}

static void httpd_led_status_poll(struct httpd_led_status_ctrl *ctrl)
{
	ulong elapsed, now, pwm_phase;
	u32 brightness_fp;
	int duty_ms;
	int i;

	if (!ctrl->led_count)
		return;

	now = get_timer(0);
	elapsed = now - ctrl->start_ms;
	if (ctrl->hw_pwm) {
		if (ctrl->last_pwm_update &&
		    now - ctrl->last_pwm_update < HTTPD_LED_HW_PWM_UPDATE_MS)
			return;

		ctrl->last_pwm_update = now;
		httpd_led_hw_pwm_set(ctrl, httpd_led_breath_fp(elapsed));
		return;
	}

	pwm_phase = elapsed % HTTPD_LED_SW_PWM_PERIOD_MS;

	for (i = 0; i < ctrl->led_count; i++) {
		brightness_fp = httpd_led_brightness_fp(ctrl, i, elapsed);
		duty_ms = (brightness_fp * HTTPD_LED_SW_PWM_PERIOD_MS +
			   HTTPD_LED_BRIGHTNESS_FP / 2) /
			  HTTPD_LED_BRIGHTNESS_FP;
		httpd_led_pin_set(&ctrl->leds[i],
				  duty_ms && pwm_phase < duty_ms);
	}
}

static void httpd_led_status_stop(struct httpd_led_status_ctrl *ctrl)
{
	int i;

	if (!ctrl->led_count)
		return;

	if (ctrl->hw_pwm) {
		httpd_led_hw_pwm_stop(ctrl);
		return;
	}

	for (i = 0; i < ctrl->led_count; i++)
		httpd_led_pin_set(&ctrl->leds[i], 0);
}

int uboot_httpd_led_start(void)
{
	int ret;

	/*
	 * The activity LED is normally one of the status LEDs. Stop the
	 * software blink first so the cyclic timer does not fight the
	 * pattern over the same pin.
	 */
	led_activity_off();

	ret = httpd_led_status_init(&httpd_led_status);
	if (ret) {
		printf("HTTP status LED pattern unavailable (%d), keeping activity LED\n",
		       ret);
		led_activity_blink();
		return -ENODEV;
	}

	httpd_led_status.active = true;

	return 0;
}

void uboot_httpd_led_poll(void)
{
	if (!httpd_led_status.active)
		return;

	httpd_led_status_poll(&httpd_led_status);
}

void uboot_httpd_led_job_start(void)
{
	if (httpd_led_status.active)
		return;

	led_activity_on();
}

void uboot_httpd_led_job_end(void)
{
	if (httpd_led_status.active)
		return;

	led_activity_blink();
}

void uboot_httpd_led_stop(void)
{
	if (!httpd_led_status.active) {
		led_activity_on();
		return;
	}

	httpd_led_status.active = false;
	httpd_led_status_stop(&httpd_led_status);
	httpd_led_status_release(&httpd_led_status);

	led_activity_on();
}

void uboot_httpd_led_dump_state(const char *tag)
{
	struct httpd_led_status_ctrl *ctrl = &httpd_led_status;
	int i;

	printf("LED-DIAG %s: active=%d led_count=%d hw_pwm=%d\n",
	       tag, ctrl->active, ctrl->led_count, ctrl->hw_pwm);

	for (i = 0; i < ctrl->led_count; i++) {
		struct httpd_led_gpio_pin *pin = &ctrl->leds[i];
		u8 gpio = pin->gpio;
		u32 data, oe, dir, flash_cfg, flash_cfg_ext;

		data = readl((void __iomem *)httpd_led_gpio_data_reg(gpio));
		oe = readl((void __iomem *)httpd_led_gpio_oe_reg(gpio));
		dir = readl((void __iomem *)httpd_led_gpio_dir_reg(gpio));
		flash_cfg = readl((void __iomem *)
			(HTTPD_LED_GPIO_SYSCTL_BASE +
			 HTTPD_LED_REG_GPIO_FLASH_MODE_CFG));
		flash_cfg_ext = readl((void __iomem *)
			(HTTPD_LED_GPIO_SYSCTL_BASE +
			 HTTPD_LED_REG_GPIO_FLASH_MODE_CFG_EXT));

		printf("LED-DIAG %s led[%d] gpio=%u act_low=%d last_on=%d "
		       "data=%08x oe=%08x dir=%08x flash=%08x flash_ext=%08x\n",
		       tag, i, gpio, pin->active_low, pin->last_on,
		       data, oe, dir, flash_cfg, flash_cfg_ext);
	}
}

#else /* !CONFIG_HTTPD_LED_PATTERN */

/*
 * Boards without a multi-colour status LED set keep the plain activity LED
 * blink. The pattern engine above is only compiled in when the board opts in
 * through CONFIG_HTTPD_LED_PATTERN.
 */

int uboot_httpd_led_start(void)
{
	led_activity_blink();

	return -ENOSYS;
}

void uboot_httpd_led_poll(void)
{
}

void uboot_httpd_led_job_start(void)
{
	led_activity_on();
}

void uboot_httpd_led_job_end(void)
{
	led_activity_blink();
}

void uboot_httpd_led_stop(void)
{
	led_activity_on();
}

void uboot_httpd_led_dump_state(const char *tag)
{
	printf("LED-DIAG %s: pattern engine not compiled in\n", tag);
}

#endif /* CONFIG_HTTPD_LED_PATTERN */
