/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/logging/log.h>

#include "audio_stub.h"

LOG_MODULE_REGISTER(audio_stub, LOG_LEVEL_INF);

int audio_capture_start(void)
{
	LOG_DBG("audio_capture_start (Phase 3 stub)");
	return 0;
}

void audio_capture_stop(void)
{
	LOG_DBG("audio_capture_stop (Phase 3 stub)");
}
