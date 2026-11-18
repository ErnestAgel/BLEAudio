/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_tasks.h"
#include "audio_stub.h"
#include "audio_storage.h"
#include "ble_audio_svc.h"
#include "sys_mgr.h"

LOG_MODULE_REGISTER(audio_cap_task, LOG_LEVEL_INF);

void audio_cap_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	static uint8_t sdu[AUDIO_MAX_SDU_BYTES];
	int64_t next_ms;
	uint32_t sent_cnt = 0;
	uint32_t stored_cnt = 0;

	LOG_INF("audio_cap_task started (Phase 2: live CoC / QSPI offline store)");

	for (;;) {
		bool storing = false;      /* an audio_storage session is open */
		bool wm_reported = false;  /* EVT_QSPI_WATERMARK_95 posted for this run */

		if (audio_capture_wait_start(K_FOREVER) != 0) {
			continue;
		}

		next_ms = k_uptime_get();
		sent_cnt = 0;
		stored_cnt = 0;

		/* Absolute 20 ms cadence: no drift from processing time */
		while (audio_capture_is_running()) {
			bool synced;
			uint32_t utc = ble_audio_svc_get_utc(&synced);
			uint8_t flags = 0;

			if (sys_mgr_nr_enabled()) {
				flags |= SDU_FLAG_NR_ON;
			}
			if (synced) {
				flags |= SDU_FLAG_UTC_VALID;
			}

			uint16_t len = audio_stub_build_frame(sdu, flags, utc);

			if (ble_audio_svc_is_l2cap_ready()) {
				/* Link is up: live stream; seal any file written while offline */
				if (storing) {
					(void)audio_storage_close();
					storing = false;
				}
				if (ble_audio_svc_enqueue_live(sdu, len) == 0) {
					sent_cnt++;
					if ((sent_cnt % 50U) == 1U) {
						LOG_INF("live frames queued=%u drops=%u", sent_cnt,
							ble_audio_svc_get_drop_cnt());
					}
				}
			} else {
				/* Link down: route the frame into QSPI via the 24 KB ring_buf */
				int err = 0;

				if (!storing) {
					err = audio_storage_open(utc, synced);
					storing = (err == 0);
					if (err == -EBUSY) {
						audio_storage_count_drop();
					}
				}
				if (storing) {
					err = audio_storage_write_sdu(sdu, len);
					if (err == 0) {
						stored_cnt++;
						if ((stored_cnt % 50U) == 1U) {
							LOG_INF("offline frames stored=%u ring_drops=%u",
								stored_cnt,
								audio_storage_get_ring_drop_cnt());
						}
					}
				}
				if (err == -ENOSPC && !wm_reported) {
					wm_reported = true;
					(void)sys_mgr_post_event(EVT_QSPI_WATERMARK_95);
				}
			}

			next_ms += AUDIO_FRAME_PERIOD_MS;
			k_sleep(K_TIMEOUT_ABS_MS(next_ms));
		}

		/* Capture stopped (BTN1/GATT/OTA/watermark): flush tail, fs_sync + fs_close */
		if (storing) {
			(void)audio_storage_close();
		}
	}
}
