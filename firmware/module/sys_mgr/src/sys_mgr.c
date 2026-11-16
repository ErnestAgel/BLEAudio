/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "sys_mgr.h"
#include "hw_ui.h"
#include "ota_led.h"
#include "ota_dfu.h"
#include "audio_stub.h"

LOG_MODULE_REGISTER(sys_mgr, LOG_LEVEL_INF);

K_MSGQ_DEFINE(sys_evt_msgq, sizeof(struct sys_evt), 16, 4);
static K_MUTEX_DEFINE(lock);

static enum sys_state state = SYS_STATE_IDLE;
static bool nr_on;

enum sys_state sys_mgr_get_state(void)
{
	return state;
}

bool sys_mgr_nr_enabled(void)
{
	return nr_on;
}

int sys_mgr_post_event(enum sys_evt_type type)
{
	struct sys_evt evt = { .type = type };
	int err = k_msgq_put(&sys_evt_msgq, &evt, K_NO_WAIT);

	if (err) {
		LOG_WRN("sys_evt_msgq full, drop evt %d", type);
	}
	return err;
}

void sys_mgr_toggle_capture(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (state == SYS_STATE_OTA) {
		LOG_WRN("BTN1 ignored in OTA state");
	} else if (state == SYS_STATE_IDLE) {
		state = SYS_STATE_CAPTURING;
		audio_capture_start();
		hw_ui_led_capture(true);
		LOG_INF("state -> CAPTURING");
	} else {
		state = SYS_STATE_IDLE;
		audio_capture_stop();
		hw_ui_led_capture(false);
		LOG_INF("state -> IDLE");
	}
	k_mutex_unlock(&lock);
}

void sys_mgr_toggle_nr(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (state == SYS_STATE_OTA) {
		LOG_WRN("BTN3 ignored in OTA state");
	} else {
		nr_on = !nr_on;
		hw_ui_led_nr(nr_on);
		LOG_INF("noise reduction %s", nr_on ? "ON" : "Bypass");
	}
	k_mutex_unlock(&lock);
}

void sys_mgr_enter_ota(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (state == SYS_STATE_OTA) {
		k_mutex_unlock(&lock);
		return;
	}
	if (state == SYS_STATE_CAPTURING) {
		audio_capture_stop();
		hw_ui_led_capture(false);
	}
	state = SYS_STATE_OTA;
	ota_led_start();
	LOG_INF("state -> OTA (SMP DFU gate open)");
	k_mutex_unlock(&lock);
}

void sys_mgr_exit_ota(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (state == SYS_STATE_OTA) {
		if (ota_dfu_is_active()) {
			LOG_WRN("DFU transfer active, cannot leave OTA");
		} else {
			ota_led_stop(false);
			state = SYS_STATE_IDLE;
			LOG_INF("state -> IDLE (left OTA)");
		}
	}
	k_mutex_unlock(&lock);
}

int sys_mgr_poll_and_dispatch(int32_t timeout_ms)
{
	struct sys_evt evt;
	int err = k_msgq_get(&sys_evt_msgq, &evt, K_MSEC(timeout_ms));

	if (err) {
		return err;
	}

	switch (evt.type) {
	case EVT_BTN_CAPTURE_TOGGLE:
		sys_mgr_toggle_capture();
		break;
	case EVT_BTN_NR_TOGGLE:
		sys_mgr_toggle_nr();
		break;
	case EVT_BTN_OTA_TOGGLE:
		if (sys_mgr_get_state() == SYS_STATE_OTA) {
			sys_mgr_exit_ota();
		} else {
			sys_mgr_enter_ota();
		}
		break;
	default:
		LOG_WRN("Unknown event %d", evt.type);
		break;
	}
	return 0;
}
