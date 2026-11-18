/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_tasks.h"
#include "ble_audio_svc.h"

LOG_MODULE_REGISTER(ble_tx_task, LOG_LEVEL_INF);

void ble_tx_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("ble_tx_task started (L2CAP CoC sender)");

	for (;;) {
		/* Pops ble_tx_msgq and sends one SDU on the CoC; blocks <=100 ms */
		(void)ble_audio_svc_tx_poll(K_MSEC(100));
	}
}
