/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_tasks.h"
#include "audio_storage.h"
#include "ble_audio_svc.h"
#include "sys_mgr.h"

LOG_MODULE_REGISTER(ble_tx_task, LOG_LEVEL_INF);

static uint8_t replay_sdu[AUDIO_MAX_SDU_BYTES];

/* BTN1 (-> CAPTURING + abort_replay), OTA, link loss or CoC loss all end the replay. */
static bool replay_aborted(void)
{
	return ble_audio_svc_abort_pending() || sys_mgr_get_state() != SYS_STATE_UPLOADING ||
	       !ble_audio_svc_is_l2cap_ready();
}

/* Send one stored file as pkt_type 0x01 and delete it once every SDU has been `sent`.
 * Returns 0 (sent+deleted), -ECANCELED (aborted, file kept intact) or another -errno
 * (file skipped, kept).
 */
static int replay_one(const char *name)
{
	uint32_t frames = 0;
	int err = audio_storage_replay_open(name);

	if (err) {
		return err;
	}

	for (;;) {
		audio_sdu_hdr_t *hdr = (audio_sdu_hdr_t *)replay_sdu;
		uint16_t len;
		bool eof;

		if (replay_aborted()) {
			audio_storage_replay_close();
			return -ECANCELED;
		}
		err = audio_storage_replay_next_frame(replay_sdu, &len, &eof);
		if (err == -ENODATA) {
			break;   /* empty / unreadable file: nothing to send, drop it below */
		}
		if (err) {
			audio_storage_replay_close();
			return err;
		}

		hdr->pkt_type = PKT_TYPE_QSPI_REPLAY;
		if (eof) {
			hdr->flags |= SDU_FLAG_EOF;
		}
		do {
			err = ble_audio_svc_send_blocking_replay(replay_sdu, len);
			if (err == -ENOMEM) {
				k_sleep(K_MSEC(2));
			}
		} while (err == -ENOMEM && !replay_aborted());
		if (err) {
			audio_storage_replay_close();
			return err == -ECANCELED ? -ECANCELED : err;
		}
		frames++;
		if (eof) {
			break;
		}
	}

	/* Delete only after the stack confirmed every SDU through the `sent` callback */
	ble_audio_svc_wait_all_sent();
	if (replay_aborted()) {
		audio_storage_replay_close();
		return -ECANCELED;
	}
	LOG_INF("replay %s: %u frames sent", name, frames);
	return audio_storage_unlink_active_replay();
}

static void replay_run(void)
{
	bool failed = false;

	if (!sys_mgr_replay_begin()) {
		return;
	}
	LOG_INF("QSPI replay start (%u files pending)", audio_storage_count_pending());

	for (;;) {
		int n = audio_storage_scan();   /* boot_<cnt>_<seq> numeric first, then <utc> */

		if (n <= 0) {
			break;
		}
		for (int i = 0; i < n; i++) {
			int err = replay_one(audio_storage_scan_name(i));

			if (err == -ECANCELED) {
				LOG_WRN("QSPI replay aborted, remaining files kept");
				goto out;
			}
			if (err) {
				LOG_ERR("replay of %s failed (%d), kept", audio_storage_scan_name(i),
					err);
				failed = true;
			}
		}
		if (failed) {
			break;   /* a re-scan would hit the same bad file again */
		}
	}
	LOG_INF("QSPI replay finished (pending %u)", audio_storage_count_pending());
out:
	sys_mgr_replay_end();
}

void ble_tx_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	bool prev_ready = false;

	LOG_INF("ble_tx_task started (L2CAP CoC sender + QSPI replay)");

	for (;;) {
		bool ready = ble_audio_svc_is_l2cap_ready();

		/* CoC just came up: anything stored while offline is due for upload */
		if (ready && !prev_ready) {
			audio_storage_request_replay();
		}
		prev_ready = ready;

		/* Replay only in the idle period: never while capturing (live pkt_type 0x00) */
		if (ready && sys_mgr_get_state() == SYS_STATE_IDLE &&
		    !audio_storage_session_busy() && audio_storage_take_replay_request()) {
			if (audio_storage_count_pending() > 0) {
				replay_run();
			}
			continue;
		}

		/* Pops ble_tx_msgq and sends one SDU on the CoC; blocks <=100 ms */
		(void)ble_audio_svc_tx_poll(K_MSEC(100));
	}
}
