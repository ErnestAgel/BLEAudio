/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_tasks.h"
#include "audio_storage.h"

LOG_MODULE_REGISTER(audio_store_task, LOG_LEVEL_INF);

/* Owns the QSPI LittleFS write path: NVS boot_cnt + mount, then 16 KB ring_buf ->
 * 4 KB page writes, tail flush on session close and the 92~95 % watermark check.
 */
void audio_store_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("audio_store_task started (QSPI LittleFS)");
	audio_storage_run();   /* never returns */
}
