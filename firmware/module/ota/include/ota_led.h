/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#ifndef OTA_LED_H_
#define OTA_LED_H_

#include <stdbool.h>

/* LED3 (P0.15) breathing driven by PWM0 ch0. */
int ota_led_init(void);
void ota_led_start(void);
void ota_led_stop(bool suspend);

#endif
