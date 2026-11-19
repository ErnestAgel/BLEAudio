/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#ifndef SYS_MGR_H_
#define SYS_MGR_H_

#include <stdbool.h>
#include <stdint.h>

#include "ble_audio_types.h"

enum sys_state {
	SYS_STATE_IDLE = 0,
	SYS_STATE_CAPTURING,
	SYS_STATE_UPLOADING,  /* idle-period QSPI replay (pkt_type 0x01) in progress */
	SYS_STATE_OTA,
};

enum sys_evt_type {
	EVT_BTN_CAPTURE_TOGGLE = 0,
	EVT_BTN_NR_TOGGLE,
	EVT_BTN_OTA_TOGGLE,
	EVT_GATT_CTRL_CMD,   /* opcode/param from GATT 0x1002 */
	EVT_STATUS_CHANGED,   /* BLE link / PHY / CoC state changed */
	EVT_QSPI_WATERMARK_95, /* audio_storage: QSPI used >= 92~95 %, force-stop capture */
	EVT_UPLOAD_DONE,      /* ble_tx_task: replay loop finished or aborted */
	EVT_AUDIO_ERR,        /* audio_capture: DMIC read timeout / hardware fault */
};

struct sys_evt {
	enum sys_evt_type type;
	uint8_t opcode;
	uint32_t param;
};

enum sys_state sys_mgr_get_state(void);
bool sys_mgr_nr_enabled(void);

/* Post an asynchronous event to sys_mgr_task's message queue. */
int sys_mgr_post_event(enum sys_evt_type type);

/* Post a validated GATT Control command (ble_ctrl_op_t) to sys_mgr_task. */
int sys_mgr_post_ctrl(uint8_t opcode, uint32_t param);

/* Fill the 10 B Status packet (ICD sys_state values). */
void sys_mgr_fill_status(ble_status_pkt_t *st);
uint8_t sys_mgr_get_codec(void);

/* Block for up to timeout_ms waiting for an event and dispatch it.
 * Returns 0 if an event was processed, -EAGAIN on timeout.
 */
int sys_mgr_poll_and_dispatch(int32_t timeout_ms);

/* Replay arbitration (ble_tx_task). begin: IDLE -> UPLOADING only if the CoC is up and
 * clears a stale abort flag. end: UPLOADING -> IDLE (no-op if BTN1 already switched to
 * CAPTURING). */
bool sys_mgr_replay_begin(void);
void sys_mgr_replay_end(void);

/* Direct state helpers (kept for synchronous callers if needed). */
void sys_mgr_start_capture(void);
void sys_mgr_stop_capture(void);
void sys_mgr_toggle_capture(void);
void sys_mgr_toggle_nr(void);
void sys_mgr_enter_ota(void);
void sys_mgr_exit_ota(void);

#endif
