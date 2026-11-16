/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/pm/device.h>
#include <zephyr/logging/log.h>

#include "ota_led.h"

LOG_MODULE_REGISTER(ota_led, LOG_LEVEL_INF);

#define STEP_MS 25

static const struct pwm_dt_spec pwm_led = PWM_DT_SPEC_GET(DT_ALIAS(ota_pwm_led));

/* sin^2 table, 0..1000 permille, 32 steps -> 0.8 s breathing cycle */
static const uint16_t lut[] = {
	0, 10, 41, 90, 156, 236, 326, 424, 525, 625, 720, 806, 879, 937, 977, 997,
	997, 977, 937, 879, 806, 720, 625, 525, 424, 326, 236, 156, 90, 41, 10, 0,
};

static struct k_work_delayable breathe_work;
static uint32_t idx;
static bool running;

static void set_duty(uint32_t permille)
{
	int err = pwm_set_dt(&pwm_led, pwm_led.period, (pwm_led.period * permille) / 1000U);

	if (err) {
		LOG_ERR("pwm_set_dt failed (%d)", err);
	}
}

static void breathe_handler(struct k_work *work)
{
	if (!running) {
		return;
	}
	set_duty(lut[idx]);
	idx = (idx + 1U) % ARRAY_SIZE(lut);
	k_work_schedule(&breathe_work, K_MSEC(STEP_MS));
}

int ota_led_init(void)
{
	if (!pwm_is_ready_dt(&pwm_led)) {
		LOG_ERR("PWM0 not ready");
		return -ENODEV;
	}
	k_work_init_delayable(&breathe_work, breathe_handler);
	return 0;
}

void ota_led_start(void)
{
	if (running) {
		return;
	}
	pm_device_action_run(pwm_led.dev, PM_DEVICE_ACTION_RESUME); /* no-op if active */
	running = true;
	idx = 0;
	k_work_schedule(&breathe_work, K_NO_WAIT);
}

void ota_led_stop(bool suspend)
{
	running = false;
	k_work_cancel_delayable(&breathe_work);
	set_duty(0);
	if (suspend) {
		pm_device_action_run(pwm_led.dev, PM_DEVICE_ACTION_SUSPEND);
	}
}
