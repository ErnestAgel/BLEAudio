# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ble_audio)

target_include_directories(app PRIVATE
  task/include
  module/sys_mgr/include
  module/hw/include
  module/ota/include
  module/ble/include
  module/audio/include
)

target_sources(app PRIVATE
  src/main.c
  task/sys_mgr_task.c
  task/ble_tx_task.c
  task/audio_cap_task.c
  task/audio_dsp_task.c
  task/audio_store_task.c
  module/sys_mgr/src/sys_mgr.c
  module/hw/src/hw_ui.c
  module/ota/src/ota_led.c
  module/ota/src/ota_dfu.c
  module/ble/src/ble_adv.c
  module/audio/src/audio_stub.c
)
