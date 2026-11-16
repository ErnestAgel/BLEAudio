/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <hal/nrf_power.h>
#include <zephyr/logging/log.h>

#include "app_tasks.h"
#include "sys_mgr.h"
#include "hw_ui.h"
#include "ota_led.h"
#include "ota_dfu.h"
#include "ble_adv.h"

LOG_MODULE_REGISTER(sys_mgr_task, LOG_LEVEL_INF);

#define WDT_NODE       DT_ALIAS(watchdog0)
/* Full-slot NVMC erase (~10 s on 464 KB) stalls CPU; keep WDT > 10 s */
#define WDT_TIMEOUT_MS 15000
#define WDT_FEED_MS    500

void sys_mgr_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int err;
	int wdt_id = -1;
	uint32_t resetreas = nrf_power_resetreas_get(NRF_POWER);

	nrf_power_resetreas_clear(NRF_POWER, resetreas);
	LOG_INF("sys_mgr_task started, state=%d RESETREAS=0x%08x",
		sys_mgr_get_state(), resetreas);

	err = hw_ui_init();
	if (err) {
		LOG_ERR("hw_ui_init failed (%d)", err);
		return;
	}

	ota_dfu_init();

	/* Self-check: pwm0 + Bluetooth, then confirm (never confirm on failure) */
	err = ota_led_init();
	if (err) {
		goto selfcheck_failed;
	}

	err = ble_adv_init();
	if (err) {
		goto selfcheck_failed;
	}

	if (IS_ENABLED(CONFIG_TASK_WDT) && DT_NODE_HAS_STATUS_OKAY(WDT_NODE)) {
		const struct device *wdt = DEVICE_DT_GET(WDT_NODE);

		if (task_wdt_init(wdt) == 0) {
			wdt_id = task_wdt_add(WDT_TIMEOUT_MS, NULL, NULL);
		}
	}

	if (ota_dfu_confirm_image() != 0) {
		goto selfcheck_failed;
	}

	for (;;) {
		(void)sys_mgr_poll_and_dispatch(WDT_FEED_MS);
		if (wdt_id >= 0) {
			task_wdt_feed(wdt_id);
		}
	}

selfcheck_failed:
	LOG_ERR("Self-check failed (%d): image NOT confirmed", err);
	hw_ui_led_error(true);
}
