/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#ifndef BLE_ADV_H_
#define BLE_ADV_H_

/* bt_enable() + connectable advertising (name + SMP UUID).
 * LED2: 1 Hz blink while advertising, solid while connected.
 */
int ble_adv_init(void);

#endif
