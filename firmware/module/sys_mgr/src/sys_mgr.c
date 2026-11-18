/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <string.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include "sys_mgr.h"
#include "hw_ui.h"
#include "ota_led.h"
#include "ota_dfu.h"
#include "audio_stub.h"
#include "ble_audio_svc.h"

LOG_MODULE_REGISTER(sys_mgr, LOG_LEVEL_INF);

K_MSGQ_DEFINE(sys_evt_msgq, sizeof(struct sys_evt), 16, 4);
static K_MUTEX_DEFINE(lock);

static enum sys_state state = SYS_STATE_IDLE;
static bool nr_on;
static uint8_t codec = CODEC_RAW_PCM;
static ble_status_pkt_t last_status;
static int64_t last_status_ms;

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

/* caller holds lock */
static void start_capture_locked(void)
{
	if (state == SYS_STATE_OTA) {
		LOG_WRN("Start capture ignored in OTA state");
	} else if (state == SYS_STATE_IDLE) {
		state = SYS_STATE_CAPTURING;
		/* BTN1 preempts any in-progress replay */
		ble_audio_svc_abort_replay();
		audio_capture_start();
		hw_ui_led_capture(true);
		LOG_INF("state -> CAPTURING");
	}
}

static void stop_capture_locked(void)
{
	if (state == SYS_STATE_CAPTURING) {
		state = SYS_STATE_IDLE;
		audio_capture_stop();
		hw_ui_led_capture(false);
		LOG_INF("state -> IDLE");
	}
}

void sys_mgr_start_capture(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	start_capture_locked();
	k_mutex_unlock(&lock);
}

void sys_mgr_stop_capture(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	stop_capture_locked();
	k_mutex_unlock(&lock);
}

void sys_mgr_toggle_capture(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (state == SYS_STATE_OTA) {
		LOG_WRN("BTN1 ignored in OTA state");
	} else if (state == SYS_STATE_IDLE) {
		start_capture_locked();
	} else {
		stop_capture_locked();
	}
	k_mutex_unlock(&lock);
}

int sys_mgr_post_ctrl(uint8_t opcode, uint32_t param)
{
	struct sys_evt evt = { .type = EVT_GATT_CTRL_CMD, .opcode = opcode, .param = param };
	int err = k_msgq_put(&sys_evt_msgq, &evt, K_NO_WAIT);

	if (err) {
		LOG_WRN("sys_evt_msgq full, drop ctrl op 0x%02x", opcode);
	}
	return err;
}

uint8_t sys_mgr_get_codec(void)
{
	return codec;
}

void sys_mgr_fill_status(ble_status_pkt_t *st)
{
	memset(st, 0, sizeof(*st));
	switch (state) {
	case SYS_STATE_CAPTURING:
		st->sys_state = ICD_SYS_STATE_CAPTURING;
		break;
	case SYS_STATE_OTA:
		st->sys_state = ICD_SYS_STATE_OTA;
		break;
	default:
		st->sys_state = ICD_SYS_STATE_IDLE;
		break;
	}
	st->nr_on = nr_on;
	st->codec_type = codec;
	st->phy_mode = ble_audio_svc_get_phy_mode();
	st->drop_cnt = sys_cpu_to_le32(ble_audio_svc_get_drop_cnt());
}

/* Notify Status on change, and once per second while capturing. */
static void status_tick(void)
{
	ble_status_pkt_t st;
	int64_t now = k_uptime_get();
	bool changed;

	sys_mgr_fill_status(&st);
	changed = memcmp(&st, &last_status, offsetof(ble_status_pkt_t, drop_cnt)) != 0;
	if (changed || (state == SYS_STATE_CAPTURING && (now - last_status_ms) >= 1000)) {
		last_status = st;
		last_status_ms = now;
		(void)ble_audio_svc_notify_status(&st);
	}
}

static void handle_ctrl(uint8_t opcode, uint32_t param)
{
	switch (opcode) {
	case CTRL_OP_START_CAPTURE:
		sys_mgr_start_capture();
		break;
	case CTRL_OP_STOP_CAPTURE:
		sys_mgr_stop_capture();
		break;
	case CTRL_OP_SET_NR:
		k_mutex_lock(&lock, K_FOREVER);
		if (state != SYS_STATE_OTA) {
			nr_on = param != 0;
			hw_ui_led_nr(nr_on);
			LOG_INF("noise reduction %s (GATT)", nr_on ? "ON" : "Bypass");
		}
		k_mutex_unlock(&lock);
		break;
	case CTRL_OP_SET_CODEC:
		k_mutex_lock(&lock, K_FOREVER);
		if (state == SYS_STATE_IDLE) {
			codec = (uint8_t)param;
		}
		k_mutex_unlock(&lock);
		break;
	case CTRL_OP_TRIGGER_REPLAY:
		LOG_WRN("Replay requested: QSPI storage arrives in Phase 3, ignored");
		break;
	default:
		LOG_WRN("Unhandled ctrl opcode 0x%02x", opcode);
		break;
	}
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
		status_tick();
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
	case EVT_GATT_CTRL_CMD:
		handle_ctrl(evt.opcode, evt.param);
		break;
	case EVT_STATUS_CHANGED:
		break;
	default:
		LOG_WRN("Unknown event %d", evt.type);
		break;
	}
	status_tick();
	return 0;
}
