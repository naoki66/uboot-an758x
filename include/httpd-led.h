/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Status LED pattern used by the U-Boot HTTP management service.
 */

#ifndef __HTTPD_LED_H__
#define __HTTPD_LED_H__

/**
 * uboot_httpd_led_start() - hand the status LEDs over to the pattern engine
 *
 * Stops the plain single-LED activity blink, collects every status LED that
 * the board device tree declares and starts the pattern.
 *
 * Return: 0 if a pattern is running, -ENODEV if no status LED was found (in
 * which case the caller keeps the legacy activity LED behaviour).
 */
int uboot_httpd_led_start(void);

/**
 * uboot_httpd_led_poll() - advance the pattern
 *
 * Call this from the network polling loop. It is rate limited internally, so
 * calling it in a tight loop is fine.
 */
void uboot_httpd_led_poll(void);

/**
 * uboot_httpd_led_stop() - release the status LEDs
 *
 * Turns every LED off and restores the SoC LED PWM mux to its previous state.
 */
void uboot_httpd_led_stop(void);

/**
 * uboot_httpd_led_job_start() - a captured job is about to run
 * uboot_httpd_led_job_end() - a captured job has finished
 *
 * Only meaningful for the legacy single activity LED; the pattern engine
 * animates continuously and ignores these.
 */
void uboot_httpd_led_job_start(void);
void uboot_httpd_led_job_end(void);

/**
 * uboot_httpd_led_dump_state() - print LED engine and pad register snapshot
 *
 * Dumps the pattern-engine state (active flag, per-LED cached last_on) and the
 * raw GPIO pad registers (data, output-enable, direction, flash-mode) so the
 * caller can compare before/after a NAND operation that is suspected of
 * clobbering the status LED.
 *
 * @tag: label prefixed to every diagnostic line (e.g. "pre-bl2", "post-bl2")
 */
void uboot_httpd_led_dump_state(const char *tag);

#endif /* __HTTPD_LED_H__ */
