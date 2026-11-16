/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * BLEAudio firmware - Phase 1A entry point.
 * src/ only contains main.c, responsible for basic RTOS task creation.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_tasks.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

K_THREAD_STACK_DEFINE(sys_mgr_stack, TASK_STACK_SYS_MGR);
K_THREAD_STACK_DEFINE(ble_tx_stack, TASK_STACK_BLE_TX);
K_THREAD_STACK_DEFINE(audio_cap_stack, TASK_STACK_AUDIO_CAP);
K_THREAD_STACK_DEFINE(audio_dsp_stack, TASK_STACK_AUDIO_DSP);
K_THREAD_STACK_DEFINE(audio_store_stack, TASK_STACK_AUDIO_STORE);

static struct k_thread sys_mgr_tcb;
static struct k_thread ble_tx_tcb;
static struct k_thread audio_cap_tcb;
static struct k_thread audio_dsp_tcb;
static struct k_thread audio_store_tcb;

int main(void)
{
	LOG_INF("BLEAudio Phase 1A booting: creating tasks");

	k_thread_create(&sys_mgr_tcb, sys_mgr_stack, K_THREAD_STACK_SIZEOF(sys_mgr_stack),
			sys_mgr_task_entry, NULL, NULL, NULL,
			TASK_PRIO_SYS_MGR, 0, K_NO_WAIT);
	k_thread_name_set(&sys_mgr_tcb, "sys_mgr");

	k_thread_create(&ble_tx_tcb, ble_tx_stack, K_THREAD_STACK_SIZEOF(ble_tx_stack),
			ble_tx_task_entry, NULL, NULL, NULL,
			TASK_PRIO_BLE_TX, 0, K_NO_WAIT);
	k_thread_name_set(&ble_tx_tcb, "ble_tx");

	k_thread_create(&audio_cap_tcb, audio_cap_stack, K_THREAD_STACK_SIZEOF(audio_cap_stack),
			audio_cap_task_entry, NULL, NULL, NULL,
			TASK_PRIO_AUDIO_CAP, 0, K_NO_WAIT);
	k_thread_name_set(&audio_cap_tcb, "audio_cap");

	k_thread_create(&audio_dsp_tcb, audio_dsp_stack, K_THREAD_STACK_SIZEOF(audio_dsp_stack),
			audio_dsp_task_entry, NULL, NULL, NULL,
			TASK_PRIO_AUDIO_DSP, 0, K_NO_WAIT);
	k_thread_name_set(&audio_dsp_tcb, "audio_dsp");

	k_thread_create(&audio_store_tcb, audio_store_stack,
			K_THREAD_STACK_SIZEOF(audio_store_stack),
			audio_store_task_entry, NULL, NULL, NULL,
			TASK_PRIO_AUDIO_STORE, 0, K_NO_WAIT);
	k_thread_name_set(&audio_store_tcb, "audio_store");

	return 0;
}
