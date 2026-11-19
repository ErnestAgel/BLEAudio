/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_tasks.h"
#include "audio_capture.h"
#include "sys_mgr.h"

LOG_MODULE_REGISTER(audio_cap_task, LOG_LEVEL_INF);

K_MSGQ_DEFINE(dsp_out_q, sizeof(void *), AUDIO_DSP_QUEUE_DEPTH, sizeof(void *));

void audio_cap_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (audio_capture_init(&dsp_out_q) != 0) {
		LOG_ERR("audio_capture_init failed: DMIC not ready");
		return;
	}

	LOG_INF("audio_cap_task started (Phase 3: PDM0 16 kHz stereo DMA -> dsp_out_q)");

	for (;;) {
		if (audio_capture_wait_start(K_FOREVER) != 0) {
			continue;
		}

		int err = audio_capture_stream_start();

		if (err != 0) {
			LOG_ERR("PDM0 stream start failed (%d)", err);
			audio_capture_stop();
			(void)sys_mgr_post_event(EVT_AUDIO_ERR);
			continue;
		}

		uint32_t discard_cnt = AUDIO_STARTUP_DISCARD_FRAMES;
		uint32_t cap_frames = 0;

		while (audio_capture_is_running()) {
			void *blk = NULL;
			size_t sz = 0;

			err = audio_capture_read_block(&blk, &sz, AUDIO_DMIC_READ_TIMEOUT_MS);
			if (err != 0) {
				if (audio_capture_is_running()) {
					LOG_ERR("dmic_read error/timeout (%d)", err);
					audio_capture_stop();
					(void)sys_mgr_post_event(EVT_AUDIO_ERR);
				}
				break;
			}

			/* Discard first 40 ms (2 frames) startup transient */
			if (discard_cnt > 0U) {
				discard_cnt--;
				audio_capture_release_block(blk);
				continue;
			}

			if (sz != AUDIO_RX_BLOCK_BYTES) {
				LOG_WRN("Unexpected DMIC block size %zu (expected %u)", sz,
					AUDIO_RX_BLOCK_BYTES);
				audio_capture_release_block(blk);
				continue;
			}

			if (audio_capture_post_block(blk, K_NO_WAIT) != 0) {
				audio_capture_release_block(blk);
				LOG_WRN("dsp_out_q full, dropped 20 ms block");
			} else {
				cap_frames++;
			}
		}

		audio_capture_stream_stop();

		/* Push end-of-session sentinel so audio_dsp_task closes QSPI & resets DSP */
		void *eos = NULL;

		(void)k_msgq_put(&dsp_out_q, &eos, K_FOREVER);
		LOG_INF("Capture session stopped (%u stereo blocks forwarded)", cap_frames);
	}
}
