/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>
#include <zephyr/logging/log.h>

#include "ota_dfu.h"
#include "ota_led.h"
#include "sys_mgr.h"
#include "audio_capture.h"
#include "audio_storage.h"

LOG_MODULE_REGISTER(ota_dfu, LOG_LEVEL_INF);

static volatile bool dfu_active;

bool ota_dfu_is_active(void)
{
	return dfu_active;
}

static enum mgmt_cb_return img_cb(uint32_t event, enum mgmt_cb_return prev_status,
				  int32_t *rc, uint16_t *group, bool *abort_more,
				  void *data, size_t data_size)
{
	switch (event) {
	case MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK:
		if (sys_mgr_get_state() != SYS_STATE_OTA) {
			/* Flash erase/write stalls the CPU: refuse unless BTN4 opened the gate */
			LOG_WRN("DFU chunk rejected: not in OTA state");
			*rc = MGMT_ERR_EACCESSDENIED;
			return MGMT_CB_ERROR_RC;
		}
		if (!dfu_active) {
			LOG_INF("DFU chunk accepted in OTA state");
		}
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_STARTED:
		dfu_active = true;
		LOG_INF("DFU started");
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED:
		/* Stay in OTA state so the transfer can be retried */
		dfu_active = false;
		LOG_WRN("DFU stopped");
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_PENDING:
		dfu_active = false;
		LOG_INF("DFU image written, pending reboot");
		break;
	default:
		break;
	}
	return MGMT_CB_OK;
}

static enum mgmt_cb_return os_reset_cb(uint32_t event, enum mgmt_cb_return prev_status,
				       int32_t *rc, uint16_t *group, bool *abort_more,
				       void *data, size_t data_size)
{
	if (sys_mgr_get_state() == SYS_STATE_CAPTURING) {
		LOG_WRN("Reset rejected while capturing");
		*rc = MGMT_ERR_EACCESSDENIED;
		return MGMT_CB_ERROR_RC;
	}
	LOG_INF("SMP OS reset requested (state=%d)", sys_mgr_get_state());
	/* Power down PWM0/LED3 before the reboot; mcumgr delays the reset itself */
	ota_led_stop(true);
	return MGMT_CB_OK;
}

static struct mgmt_callback img_mgmt_cb = {
	.callback = img_cb,
	.event_id = MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK | MGMT_EVT_OP_IMG_MGMT_DFU_STARTED |
		    MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED | MGMT_EVT_OP_IMG_MGMT_DFU_PENDING,
};

static struct mgmt_callback os_mgmt_cb = {
	.callback = os_reset_cb,
	.event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};

void ota_dfu_init(void)
{
	mgmt_callback_register(&img_mgmt_cb);
	mgmt_callback_register(&os_mgmt_cb);
}

int ota_dfu_selfcheck(void)
{
	/* LittleFS on QSPI must mount (NVS boot_cnt + fs_mount done by audio_store_task) */
	int err = audio_storage_selfcheck();

	if (err) {
		LOG_ERR("Self-check: LittleFS mount check FAILED (%d)", err);
		return err;
	}
	LOG_INF("Self-check: LittleFS mount OK");

	/* PDM0 DMIC device must be ready before confirming a new image */
	err = audio_capture_selfcheck();
	if (err) {
		LOG_ERR("Self-check: DMIC ready check FAILED (%d)", err);
		return err;
	}
	return 0;
}

int ota_dfu_confirm_image(void)
{
	int fs_err = ota_dfu_selfcheck();

	if (fs_err) {
		/* Never confirm an image whose storage self-check failed; even an
		 * already-confirmed image must report failure so LED4 indicates error.
		 */
		return fs_err;
	}
	if (boot_is_img_confirmed()) {
		return 0;
	}
	int err = boot_write_img_confirmed();

	if (err) {
		LOG_ERR("boot_write_img_confirmed failed (%d)", err);
	} else {
		LOG_INF("New image confirmed");
	}
	return err;
}
