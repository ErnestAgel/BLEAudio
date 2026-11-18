/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * ble_audio_svc: custom GATT service (0x1002/0x1003/0x1004), post-connect
 * 6-step negotiation, CTS client and L2CAP CoC (PSM 0x0080) audio server.
 */
#ifndef BLE_AUDIO_SVC_H_
#define BLE_AUDIO_SVC_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "ble_audio_types.h"

#define BLE_AUDIO_L2CAP_PSM 0x0080U
#define BLE_AUDIO_MAX_SDU   AUDIO_MAX_SDU_BYTES

/* Call once after bt_enable(). Registers L2CAP server, CTS client, GATT callbacks. */
int ble_audio_svc_init(void);

bool ble_audio_svc_is_connected(void);
bool ble_audio_svc_is_l2cap_ready(void);
uint8_t ble_audio_svc_get_phy_mode(void);
uint32_t ble_audio_svc_get_drop_cnt(void);

/* UTC (UNIX s). *is_synced=false -> value is monotonic uptime seconds. */
uint32_t ble_audio_svc_get_utc(bool *is_synced);

/* Producer side (any thread). Copies SDU into a net_buf and queues it (depth 8).
 * Queue/pool full -> Drop-Oldest + drop_cnt++. -ENOTCONN if the CoC is not up.
 */
int ble_audio_svc_enqueue_live(const uint8_t *sdu, uint16_t len);

/* Consumer side, called only from ble_tx_task: pops one SDU and sends it on the
 * CoC, honouring the in-flight limit. Blocks at most `timeout`.
 */
int ble_audio_svc_tx_poll(k_timeout_t timeout);

int ble_audio_svc_send_blocking_replay(const uint8_t *sdu, uint16_t len);
void ble_audio_svc_abort_replay(void);
/* Re-arm replay: called by sys_mgr_replay_begin() under the sys_mgr lock. */
void ble_audio_svc_clear_abort(void);
bool ble_audio_svc_abort_pending(void);
void ble_audio_svc_wait_all_sent(void);

/* Notify Status (skipped silently if CCCD not subscribed). */
int ble_audio_svc_notify_status(const ble_status_pkt_t *st);

#endif /* BLE_AUDIO_SVC_H_ */
