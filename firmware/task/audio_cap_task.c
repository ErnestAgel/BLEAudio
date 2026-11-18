/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_tasks.h"
#include "audio_stub.h"
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

	LOG_INF("audio_cap_task started (Phase 1B simulated 20 ms frames)");

	for (;;) {
		if (audio_capture_wait_start(K_FOREVER) != 0) {
			continue;
		}

		next_ms = k_uptime_get();
		sent_cnt = 0;

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
			int err = ble_audio_svc_enqueue_live(sdu, len);

			if (err == 0) {
				sent_cnt++;
			}
			if ((sent_cnt % 50U) == 1U && err == 0) {
				LOG_INF("live frames queued=%u drops=%u", sent_cnt,
					ble_audio_svc_get_drop_cnt());
			}

			next_ms += AUDIO_FRAME_PERIOD_MS;
			k_sleep(K_TIMEOUT_ABS_MS(next_ms));
		}
	}
}
