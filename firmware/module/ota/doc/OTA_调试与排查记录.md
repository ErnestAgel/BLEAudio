# BLE OTA（MCUboot Direct-XIP + Revert）调试与排查记录

**适用模块**：`firmware/module/ota/`（`ota_dfu`、`ota_led`）  
**目标平台**：nRF52840 DK (`nrf52840dk/nrf52840`)，NCS v3.4.2 / Zephyr v4.4.2  
**基线版本**：Phase 1A（`v0.1.0` -> `v0.1.2` -> `v0.1.3`）  
**排查流程图**：[OTA_调试与排查流程.drawio](../../../../assets/OTA_调试与排查流程.drawio)

---

## 1. 现象与排查时间线

在完成 `MCUboot Direct-XIP + Revert` 工程搭建与 `module/task` 模块化拆分后，进行手机端 BLE OTA 实测时依次遇到并定位了以下 3 个典型问题：

| 阶段 | 现场现象 / RTT 关键日志 | 定位原因 | 解决措施 |
| :--- | :--- | :--- | :--- |
| **问题 1** | 手机连接后立即报错：`DFU chunk rejected: not in OTA state` -> `DFU stopped` | **非 OTA 态安全门控生效**：未按 `BTN4` 进入 `SYS_STATE_OTA`，`ota_dfu.c` 中的 `MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK` Hook 按设计拒绝写入，防止音频/非升级态误触发片内 Flash 擦写。 | 确认按 **`BTN4`** 进入 OTA 态（`LED3` 呼吸，日志打印 `state -> OTA`）后再发起升级。 |
| **问题 2** | 修改 `firmware/VERSION` 为 `0.1.1` 后增量编译，升级完成重启仍显示 `v0.1.0` | **增量构建未重打 `dfu_application.zip`**：检查 `dfu_application.zip` 内 `manifest.json`，`version_MCUBOOT+XIP` 仍为 `0.1.0+0`。Direct-XIP 启动时比较两槽版本号，因版本未严格高于旧槽而继续从 Slot 0 启动。 | 修改 `VERSION` 后执行 **Pristine Build**（`./build.ps1 -Pristine`），确认 `manifest.json` 内 Slot 0 / Slot 1 均为新版本号。 |
| **问题 3** | 进入 OTA 态并开始传输后，板子在写入中途反复重启；重启后日志打印 `RESETREAS=0x00000002` | **高优先级 MCUmgr 工作队列饿死看门狗喂狗线程**：开启 `CONFIG_LOG_MODE_IMMEDIATE=y` 后定位到 `ota_dfu: DFU started` 后连续执行 `nrf_flash_sync_exe`（单页擦除 `89.7 ms` + 写入 `7.5 ms`）。`MCUMGR_TRANSPORT_WORKQUEUE` 默认优先级为 **`3`**，在连续 ~12.3 s 的 OTA 传输期间频繁抢占并饿死优先级为 **`8`** 的 `sys_mgr_task`，导致 `task_wdt` 超时触发硬件看门狗复位（`RESETREAS` bit1 = `DOG`）。 | 1. 将 `WDT_TIMEOUT_MS` 设为 `15000`（覆盖整槽/多页擦写窗口）；<br>2. 将 `CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_THREAD_PRIO` 设为 **`9`**（低于 `sys_mgr_task` 优先级 `8`）；<br>3. 将 `CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_STACK_SIZE` 增至 `4096`。 |

---

## 2. 关键排查思路与验证手段

### 2.1 硬件复位源定位（`RESETREAS`）
在 `firmware/task/sys_mgr_task.c` 启动时读取并清除 `NRF_POWER->RESETREAS`：
- `0x00000001` (`RESETPIN`)：板载 `RESET` 按键复位；
- `0x00000002` (`DOG`)：**硬件看门狗超时复位**（直接锁定问题与 `task_wdt` 相关，而非 HardFault）；
- `0x00000004` (`SREQ`)：软件复位（正常 OTA 完成后 `OS_MGMT_RESET` 触发的复位）。

### 2.2 即时日志定位卡死/超时点（`CONFIG_LOG_MODE_IMMEDIATE=y`）
Zephyr 默认使用 `CONFIG_LOG_MODE_DEFERRED=y`，日志先缓存再由低优先级日志线程（优先级 `14`）异步刷出。当高优先级线程持续占用 CPU 时，复位前的最后几条日志会丢失。  
临时开启 `CONFIG_LOG_MODE_IMMEDIATE=y` 及 `CONFIG_FLASH_LOG_LEVEL_DBG=y` 后，成功捕获到真实执行序列：
```text
[00:00:15.919,036] <inf> ota_dfu: DFU chunk accepted in OTA state
[00:00:15.919,372] <inf> ota_dfu: DFU started
[00:00:15.919,860] <dbg> flash_sync_mpsl: nrf_flash_sync_exe: duration: 89700
...
[00:00:28.275,482] <inf> ota_dfu: DFU image written, pending reboot
[00:00:28.379,211] <dbg> mcumgr_img_grp: img_mgmt_set_next_boot_slot: (1, test)
[00:00:28.498,474] <inf> ota_dfu: SMP OS reset requested (state=2)
*** Booting My Application v0.1.3-f6a9c6afc9ed ***
...
[00:00:00.001,190] <inf> sys_mgr_task: sys_mgr_task started, state=0 RESETREAS=0x00000004
[00:00:00.016,571] <inf> ota_dfu: New image confirmed
```

### 2.3 `boot_is_img_confirmed()` 与 `boot_write_img_confirmed()` 机制说明
- `boot_write_img_confirmed()` **不是**写固件镜像，而是向**当前正在运行的槽位**尾部写入 `Image OK` 确认标记。
- 工厂烧录的 `zephyr.signed.confirmed.hex` 或已确认固件开机时，`boot_is_img_confirmed()` 返回 `true` 直接跳过，避免每次上电重复写 Flash。
- 仅当 OTA 升级到新槽位后的**第 1 次试运行启动**时，`boot_is_img_confirmed()` 返回 `false`，`sys_mgr_task` 在自检全部通过后调用 `boot_write_img_confirmed()` 永久固化新版本（打印 `ota_dfu: New image confirmed`）。

---

## 3. 优先级调整对音频与看门狗的影响及硬件 DMA 边界

### 3.1 `CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_THREAD_PRIO=9` 对音频传输的影响
**不影响后续 BLE 音频传输，反而更安全：**
1. **链路完全独立**：`MCUMGR_TRANSPORT_WORKQUEUE` 仅服务于 MCUmgr SMP 协议（OTA / 设备管理）；后续音频实时传输由 `ble_tx_task`（优先级 `6`）直接调用 `bt_l2cap_chan_send()` 经 L2CAP CoC 发送，射频调度由 SDC 与 BT 协议栈线程处理，不经过 MCUmgr 工作队列。
2. **状态严格互斥**：`SYS_STATE_OTA` 与音频采集/传输互斥，且将 MCUmgr 工作队列优先级设为 `9`（低于 `audio_cap=4`、`audio_dsp=5`、`ble_tx=6`、`audio_store=7`、`sys_mgr=8`），可同时防止音频线程和看门狗喂狗线程被 SMP 任务抢占饿死。

### 3.2 OTA 过程是否使用 DMA？
- **BLE 空中收发阶段使用 DMA**：nRF52840 的 2.4 GHz `RADIO` 通过 **EasyDMA** 将接收到的 SMP 分片直接搬运到 RAM 缓冲区。
- **写入片内 Flash 阶段不使用 DMA**：nRF52840 片内 Flash 控制器（`NVMC`）没有 DMA。驱动通过 `flash_sync_mpsl`（`nrf_flash_sync_exe`）向 MPSL 申请射频空闲时隙（timeslot），在时隙内由 CPU 同步执行擦除（单页 4 KB 约 `89.7 ms`）和写入（约 `7.5 ms`），擦写期间 CPU 从片内 Flash 取指会暂停。
