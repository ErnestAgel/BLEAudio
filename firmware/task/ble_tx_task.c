/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_tasks.h"

LOG_MODULE_REGISTER(ble_tx_task, LOG_LEVEL_INF);

void ble_tx_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("ble_tx_task started (Phase 1A standby)");

	for (;;) {
		/* Phase 1B: consume ble_tx_msgq and send via L2CAP CoC */
		k_sleep(K_SECONDS(1));
	}
}
