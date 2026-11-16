/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#ifndef OTA_DFU_H_
#define OTA_DFU_H_

#include <stdbool.h>

/* Registers MCUmgr hooks: upload gate (only in SYS_STATE_OTA), DFU progress
 * tracking and reset hook.
 */
void ota_dfu_init(void);
bool ota_dfu_is_active(void);

/* "Test first, confirm later": confirm the running image only if
 * boot_is_img_confirmed() is false. Call after all self-checks passed.
 */
int ota_dfu_confirm_image(void);

#endif
