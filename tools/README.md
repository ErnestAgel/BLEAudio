# BleAudio 上位机工具集 (`tools/`)

本目录存放 **BleAudio** 项目的上位机 BLE 音频接收工具与标准化声学降噪评测脚本，配合 [`sample/`](../sample/README.md) 中的基准测试套件与实测录音完成全链路采集、还原与量化对比。

```text
tools/
├── README.md                    # 本文件：上位机工具说明、依赖安装与用法示例
├── ble_audio_recv.py            # WSL2 / BlueZ 上位机 BLE L2CAP CoC 音频接收与解包脚本
├── generate_acoustic_plots.py   # 标准化声学与降噪评估绘图脚本 (DNSMOS + SQUIM + Silero VAD + LUFS)
└── requirements.txt             # generate_acoustic_plots.py 所需 Python 依赖列表
```

---

## 1. BLE 音频接收与解码工具 (`ble_audio_recv.py`)

运行于 **WSL2 (Ubuntu / BlueZ)**，通过 `bleak` + 原生 Linux `AF_BLUETOOTH / BTPROTO_L2CAP` (`SOCK_SEQPACKET`) 连接开发板 **`BLEAudio_DK`**，完成控制信令交互、高速音频流接收与 WAV 还原。

### 1.1 核心功能

- **GATT 控制与状态订阅**：连接后自动读取 `0x1004`（`PSM` 与 `max_sdu`），通过 `0x1002` 下发 **`0x05` UTC 校时**、启停采集（`0x01/0x02`）、降噪开关（`0x03`）及编码切换（`0x06`），并实时打印 `0x1003` 10 B Status Notify。
- **L2CAP CoC 接收与补传兼容**：使用 `BT_RCVMTU` 与 `SO_RCVBUF = 4194304` 建立信用流控信道；自动校验 `uint16` `seq_num`（支持 `pkt_type = 0x00` 实时流与 `0x01` QSPI 离线补传切换时的序号重置）。
- **无状态解码与自动分轨保存**：支持 **`CODEC_RAW_PCM (0x00, 640 B)`** 与 **帧独立 `CODEC_IMA_ADPCM_IND (0x01, 164 B)`** 解码为 `16 kHz / 16-bit / 单声道` `.wav`。默认开启按帧头 `FLAG_NR`（`bit0`）实时分轨，输出主文件同时自动生成 **`<name>_bypass.wav`** 与 **`<name>_nr.wav`**，方便同场录制 `BTN3` 切换对照。

### 1.2 运行前提与常用命令

```bash
# WSL2 内核需支持蓝牙并通过 usbipd-win 透传适配器，确保 bluetoothd 正在运行
pip install bleak

# 1. 交互模式（默认保存 test_pcm.wav，并自动拆出 test_pcm_bypass.wav / test_pcm_nr.wav）
python3 tools/ble_audio_recv.py -o tools/test_pcm.wav
# 运行后可输入交互命令：
#   start | stop | nr 0 | nr 1 | codec 0 | codec 1 | quit

# 2. 定时自动录制 30 秒（强制 ADPCM 编码并开启 NR）
python3 tools/ble_audio_recv.py --codec adpcm --nr 1 --duration 30 -o capture_nr.wav

# 3. 按 MAC 直连并禁用按 FLAG_NR 自动拆分，丢帧处补静音保持时间轴
python3 tools/ble_audio_recv.py -a AA:BB:CC:DD:EE:FF --no-split-nr --fill-gaps -o full.wav
```

### 1.3 命令行参数一览

| 参数 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `-n, --name` | `BLEAudio_DK` | 按蓝牙广播名称扫描设备 |
| `-a, --address` | 无 | 直接指定 BLE MAC 地址（跳过名称扫描） |
| `-o, --output` | `ble_audio.wav` | 输出主 WAV 文件路径 |
| `--codec {pcm,adpcm}` | 不主动切换 | 连接后通过 `0x06` 设置编码（`pcm` 或 `adpcm`） |
| `--nr {0,1}` | 不主动切换 | 连接后通过 `0x03` 设置降噪开关（`0=Bypass`, `1=NR ON`） |
| `--duration <秒>` | `0`（交互模式） | 自动下发 `0x01` 开始并在 N 秒后 `0x02` 停止 |
| `--no-utc` | `False` | 跳过建链后的 `0x05` UTC 校时 |
| `--fill-gaps` | `False` | 发生空口丢帧时按 `seq_num` 差值写入静音帧补齐时间轴 |
| `--no-split-nr` | `False` | 关闭按 `FLAG_NR` 自动生成 `*_bypass.wav` 与 `*_nr.wav` |
| `--scan-timeout` | `15.0` | BLE 扫描超时时间（秒） |

---

## 2. 标准化声学与降噪评估脚本 (`generate_acoustic_plots.py`)

采用业界标准化预训练模型与客观指标（无手写主观阈值），对开发板实录或任意对比音频进行量化打分并绘制多维度诊断图表，同时支持一键重建 [`sample/assets/`](../sample/assets/) 基准图谱。

### 2.1 评估指标体系

- **Microsoft DNSMOS（ITU-T P.835 / P.808）**：`SIG`（人声保真度）、`BAK`（背景干净度）、`OVRL`（综合听感）、`P.808`（总体质量 MOS，1–5 分）。
- **Meta TorchAudio-SQUIM（免参考客观评估）**：`STOI`（可懂度 `0–1`）、`PESQ`（感知质量）、`SI-SDR`（信噪/失真比 dB）。
- **Silero VAD（ONNX）分段统计**：自动切分语音段与纯静音/停顿段（含 `150 ms` 边界保护），输出 `VAD%`、`SpRMS`（语音段 RMS）、`NsRMS`（静音底噪 RMS）与 `Sp-Ns`（语噪对比度 dB）。
- **ITU-R BS.1770 响度与频谱诊断**：计算 `LUFS`，并在对齐至 `-25 LUFS` 的同响度基准上绘制 STFT 语谱图、纯静音段/活跃语音段 Welch PSD 及 20 ms 帧能量箱线图。

### 2.2 依赖安装与常用命令

```powershell
# 建议在 Python 3.10+ 虚拟环境中安装依赖
pip install -r tools/requirements.txt

# 1. 对比实机 Bypass 与 NR ON（输出多维对比图与 Delta 量化表）
python tools/generate_acoustic_plots.py sample/hw_test/test_pcm_bypass.wav sample/hw_test/test_pcm_nr.wav --labels "Bypass" "NR ON" -o sample/assets/hw_test_compare.png

# 2. 分析单路实录 WAV（不对比 Set1 基线柱状图可加 --no-ref）
python tools/generate_acoustic_plots.py path/to/captured.wav --no-ref

# 3. 指定多声道 WAV 的右声道（例如原始立体声 PDM 录制 ch=1）
python tools/generate_acoustic_plots.py path/to/stereo_pdm.wav --channel 1

# 4. 重建 sample/assets/ 下的标准基线图谱并打印各基准音频指标
python tools/generate_acoustic_plots.py --baseline
```

### 2.3 命令行参数一览

| 参数 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `wavs` | 空（等价 `--baseline`） | 待分析的一个或多个 WAV 路径（支持通配符，自动重采样至 16 kHz 评估） |
| `--baseline` | `False` | 重新生成 `sample/assets/` 下的 3 张基准图谱并打印标准基线表 |
| `-c, --channel` | `0` | 多声道 WAV 读取的声道索引 |
| `--labels` | 文件名 stem | 与输入 WAV 一一对应的图例名称 |
| `-o, --out` | 自动命名 PNG | 自定义输出对比报告图路径 |
| `--title` | 自动生成标题 | 自定义报告总标题 |
| `--no-ref` | `False` | 隐藏柱状图中的 `Set1 纯人声` / `Office +5dB` 参考基线 |
