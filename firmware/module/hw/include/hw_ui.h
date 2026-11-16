/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#ifndef HW_UI_H_
#define HW_UI_H_

#include <stdbool.h>

/* Buttons (sw0..sw3) and GPIO LEDs (led0/led1/led3) via Zephyr DT API.
 * LED3 (led2 / P0.15) is owned exclusively by PWM0 - never touched here.
 */
int hw_ui_init(void);

void hw_ui_led_capture(bool on);   /* LED1 */
void hw_ui_led_ble(bool on);       /* LED2 */
void hw_ui_led_nr(bool on);        /* LED4 */
void hw_ui_led_error(bool on);     /* LED4, hardware init failure */

#endif
