/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_tasks.h"

LOG_MODULE_REGISTER(audio_store_task, LOG_LEVEL_INF);

void audio_store_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("audio_store_task started (Phase 2 standby)");

	for (;;) {
		/* Phase 2: QSPI LittleFS 4KB page write & watermark check */
		k_sleep(K_SECONDS(1));
	}
}
