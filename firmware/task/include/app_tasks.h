/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#ifndef APP_TASKS_H_
#define APP_TASKS_H_

/* Thread priorities and stack sizes (aligned with BLE-AUDIO-SDS-002 §2.3) */
#define TASK_PRIO_AUDIO_CAP   4
#define TASK_PRIO_AUDIO_DSP   5
#define TASK_PRIO_BLE_TX      6
#define TASK_PRIO_AUDIO_STORE 7
#define TASK_PRIO_SYS_MGR     8

#define TASK_STACK_AUDIO_CAP   2048
#define TASK_STACK_AUDIO_DSP   4096
#define TASK_STACK_BLE_TX      2048
#define TASK_STACK_AUDIO_STORE 2048
#define TASK_STACK_SYS_MGR     2048

void sys_mgr_task_entry(void *p1, void *p2, void *p3);
void ble_tx_task_entry(void *p1, void *p2, void *p3);
void audio_cap_task_entry(void *p1, void *p2, void *p3);
void audio_dsp_task_entry(void *p1, void *p2, void *p3);
void audio_store_task_entry(void *p1, void *p2, void *p3);

#endif
