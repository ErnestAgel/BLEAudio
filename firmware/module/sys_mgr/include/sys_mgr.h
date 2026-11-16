/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#ifndef SYS_MGR_H_
#define SYS_MGR_H_

#include <stdbool.h>
#include <stdint.h>

enum sys_state {
	SYS_STATE_IDLE = 0,
	SYS_STATE_CAPTURING,
	SYS_STATE_OTA,
};

enum sys_evt_type {
	EVT_BTN_CAPTURE_TOGGLE = 0,
	EVT_BTN_NR_TOGGLE,
	EVT_BTN_OTA_TOGGLE,
};

struct sys_evt {
	enum sys_evt_type type;
};

enum sys_state sys_mgr_get_state(void);
bool sys_mgr_nr_enabled(void);

/* Post an asynchronous event to sys_mgr_task's message queue. */
int sys_mgr_post_event(enum sys_evt_type type);

/* Block for up to timeout_ms waiting for an event and dispatch it.
 * Returns 0 if an event was processed, -EAGAIN on timeout.
 */
int sys_mgr_poll_and_dispatch(int32_t timeout_ms);

/* Direct state helpers (kept for synchronous callers if needed). */
void sys_mgr_toggle_capture(void);
void sys_mgr_toggle_nr(void);
void sys_mgr_enter_ota(void);
void sys_mgr_exit_ota(void);

#endif
