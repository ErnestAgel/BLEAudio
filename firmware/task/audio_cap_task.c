/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_tasks.h"

LOG_MODULE_REGISTER(audio_cap_task, LOG_LEVEL_INF);

void audio_cap_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("audio_cap_task started (Phase 3 standby)");

	for (;;) {
		/* Phase 3: PDM0 dmic_read() capture loop */
		k_sleep(K_SECONDS(1));
	}
}
