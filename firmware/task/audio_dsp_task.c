/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_tasks.h"
#include "audio_capture.h"
#include "audio_codec.h"
#include "audio_dsp.h"
#include "audio_storage.h"
#include "ble_audio_svc.h"
#include "sys_mgr.h"

LOG_MODULE_REGISTER(audio_dsp_task, LOG_LEVEL_INF);

void audio_dsp_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	static int16_t mono_pcm[AUDIO_FRAME_SAMPLES];
	static uint8_t sdu[AUDIO_MAX_SDU_BYTES];
	uint16_t seq_num = 0;
	uint32_t sent_cnt = 0;
	uint32_t stored_cnt = 0;
	bool storing = false;
	bool wm_reported = false;

	(void)audio_dsp_init();
	LOG_INF("audio_dsp_task started (Phase 3: GSC/NLMS/VAD -> Codec -> CoC/QSPI)");

	for (;;) {
		void *blk = NULL;

		if (k_msgq_get(&dsp_out_q, &blk, K_FOREVER) != 0) {
			continue;
		}

		if (blk == NULL) {
			/* Capture stopped: flush/close any open QSPI file and reset session */
			if (storing) {
				(void)audio_storage_close();
				storing = false;
			}
			audio_dsp_reset();
			seq_num = 0;
			sent_cnt = 0;
			stored_cnt = 0;
			wm_reported = false;
			continue;
		}

		bool vad = false;
		int err = audio_dsp_process((const int16_t *)blk, mono_pcm, &vad);

		/* Return DMA slab block immediately after DSP consumes stereo samples */
		audio_capture_release_block(blk);
		if (err != 0) {
			continue;
		}

		bool synced = false;
		uint32_t utc = ble_audio_svc_get_utc(&synced);
		uint8_t flags = 0;

		if (audio_dsp_get_nr_enable()) {
			flags |= SDU_FLAG_NR_ON;
		}
		if (vad) {
			flags |= SDU_FLAG_VAD;
		}
		if (synced) {
			flags |= SDU_FLAG_UTC_VALID;
		}

		enum codec_type codec = (enum codec_type)sys_mgr_get_codec();
		uint16_t len = 0;

		err = audio_codec_encode_sdu(codec, mono_pcm, seq_num++, PKT_TYPE_LIVE_STREAM,
					     flags, utc, sdu, &len);
		if (err != 0) {
			LOG_ERR("audio_codec_encode_sdu failed (%d)", err);
			continue;
		}

		if (ble_audio_svc_is_l2cap_ready()) {
			/* Link is up: live stream; seal any file written while offline */
			if (storing) {
				(void)audio_storage_close();
				storing = false;
			}
			if (ble_audio_svc_enqueue_live(sdu, len) == 0) {
				sent_cnt++;
				if ((sent_cnt % 50U) == 1U) {
					LOG_INF("live frames queued=%u drops=%u codec=%u nr=%u vad=%u",
						sent_cnt, ble_audio_svc_get_drop_cnt(),
						(unsigned)codec,
						(unsigned)((flags & SDU_FLAG_NR_ON) != 0U),
						(unsigned)vad);
				}
			}
		} else {
			/* Link down: route the frame into QSPI via the 24 KB ring_buf */
			err = 0;

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
						LOG_INF("offline frames stored=%u ring_drops=%u codec=%u",
							stored_cnt,
							audio_storage_get_ring_drop_cnt(),
							(unsigned)codec);
					}
				}
			}
			if (err == -ENOSPC && !wm_reported) {
				wm_reported = true;
				(void)sys_mgr_post_event(EVT_QSPI_WATERMARK_95);
			}
		}
	}
}
