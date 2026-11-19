#!/usr/bin/env python3
"""BLEAudio_DK 上位机接收脚本 (WSL2 / BlueZ)。

依据 doc/05_通信与模块接口/01_软件API与BLE通信协议_ICD.md 第 3 节:
  * 扫描并连接 BLEAudio_DK (bleak -> BlueZ D-Bus)
  * 读 0x1004 (psm, max_sdu), 下发 0x05 UTC 校时
  * 建立 LE L2CAP CoC (PSM=0x0080, SOCK_SEQPACKET, 一次 recv = 一条 SDU)
  * 订阅 0x1003 Status Notify
  * 控制: 0x01 开始 / 0x02 停止 / 0x03 NR / 0x06 Codec
  * 校验 seq_num (uint16 回绕) 连续性并统计丢帧
  * Raw PCM / 帧独立 IMA-ADPCM 还原为 16 kHz 单声道 16-bit .wav
  * 默认按帧头 FLAG_NR (bit0) 自动同步拆分输出 *_bypass.wav 与 *_nr.wav (--no-split-nr 可关闭)

依赖:  pip install bleak
前提:  WSL2 内核需带蓝牙支持并通过 usbipd 透传适配器, bluetoothd 已运行。
用法:  python3 tools/ble_audio_recv.py --codec adpcm --duration 30 -o out.wav
       交互命令 (运行中输入): start | stop | nr 0/1 | codec 0/1 | quit
"""
import argparse
import asyncio
import ctypes
import ctypes.util
import os
import socket
import struct
import sys
import threading
import time
import wave

from bleak import BleakClient, BleakScanner

DEV_NAME = "BLEAudio_DK"
UUID_FMT = "6e40{:04x}-b5a3-f393-e0a9-e50e24dcca9e"
UUID_CTRL = UUID_FMT.format(0x1002)
UUID_STATUS = UUID_FMT.format(0x1003)
UUID_INFO = UUID_FMT.format(0x1004)

OP_START, OP_STOP, OP_NR, OP_UTC, OP_CODEC = 0x01, 0x02, 0x03, 0x05, 0x06

HDR_FMT = "<HBBBIB"  # seq, pkt_type, codec, flags, utc, reserved
HDR_SIZE = struct.calcsize(HDR_FMT)  # 10
PCM_BYTES, ADPCM_BYTES = 640, 164
SAMPLES_PER_FRAME = 320
SAMPLE_RATE = 16000
FLAG_NR, FLAG_VAD, FLAG_EOF, FLAG_UTC = 1, 2, 4, 8
SYS_STATES = {0: "INIT", 1: "IDLE", 2: "CAPTURING", 3: "UPLOADING",
              4: "OTA", 5: "REBOOTING", 0xFF: "ERROR"}

# ---- Linux L2CAP socket (经 ctypes 以支持 LE 地址类型) ----
AF_BLUETOOTH, BTPROTO_L2CAP = 31, 0
SOL_BLUETOOTH, BT_SECURITY, BT_RCVMTU = 274, 4, 13
BDADDR_LE_PUBLIC, BDADDR_LE_RANDOM = 1, 2


class SockaddrL2(ctypes.Structure):
    _fields_ = [("l2_family", ctypes.c_ushort), ("l2_psm", ctypes.c_ushort),
                ("l2_bdaddr", ctypes.c_ubyte * 6), ("l2_cid", ctypes.c_ushort),
                ("l2_bdaddr_type", ctypes.c_ubyte)]


def l2cap_coc_connect(addr: str, addr_type: int, psm: int, rcv_mtu: int,
                      timeout: float = 10.0) -> socket.socket:
    sock = socket.socket(AF_BLUETOOTH, socket.SOCK_SEQPACKET, BTPROTO_L2CAP)
    libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
    try:
        # 先 bind 为 LE 本地地址类型 (BDADDR_ANY), 再设置 BT_RCVMTU (须在 connect 前)
        local = SockaddrL2(AF_BLUETOOTH, 0, (ctypes.c_ubyte * 6)(), 0, BDADDR_LE_PUBLIC)
        if libc.bind(sock.fileno(), ctypes.byref(local), ctypes.sizeof(local)) < 0:
            e = ctypes.get_errno()
            raise OSError(e, "bind: " + os.strerror(e))
        sock.setsockopt(SOL_BLUETOOTH, BT_RCVMTU, rcv_mtu)
        # 增大内核接收缓冲以减少 BlueZ LE_CREDITS 回包频次（规避部分内核 tx_ida 耗尽）
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4194304)
        # bdaddr 在内核中为小端 (与显示字节序相反)
        raw = bytes(int(x, 16) for x in addr.split(":"))[::-1]
        remote = SockaddrL2(AF_BLUETOOTH, socket.htobe16(psm) if False else psm,
                            (ctypes.c_ubyte * 6)(*raw), 0, addr_type)
        sock.settimeout(timeout)
        # settimeout 使 fd 为非阻塞, 此处用线程内阻塞 connect: 恢复阻塞并用 SO_SNDTIMEO
        sock.setblocking(True)
        tv = struct.pack("ll", int(timeout), 0)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDTIMEO, tv)
        if libc.connect(sock.fileno(), ctypes.byref(remote), ctypes.sizeof(remote)) < 0:
            e = ctypes.get_errno()
            raise OSError(e, "L2CAP connect: " + os.strerror(e))
        return sock
    except Exception:
        sock.close()
        raise


# ---- IMA-ADPCM 解码 (帧独立: 4B [prev:int16, idx:u8, pad] + 160B 半字节) ----
_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
         45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
         209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
         796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
         2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
         7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
         20350, 22385, 24623, 27086, 29794, 32767]
_IDX_ADJ = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_decode_frame(payload: bytes) -> bytes:
    """164B -> 320 个 int16 (640B)。首点明文 + 319 个 nibble (低半字节在前)。"""
    pred, idx, _ = struct.unpack_from("<hBB", payload, 0)
    idx = min(max(idx, 0), 88)
    out = [pred]
    for k in range(SAMPLES_PER_FRAME - 1):
        b = payload[4 + (k >> 1)]
        nib = (b & 0x0F) if (k & 1) == 0 else (b >> 4)
        step = _STEP[idx]
        diff = step >> 3
        if nib & 1: diff += step >> 2
        if nib & 2: diff += step >> 1
        if nib & 4: diff += step
        pred += -diff if nib & 8 else diff
        pred = max(-32768, min(32767, pred))
        idx = min(max(idx + _IDX_ADJ[nib & 7], 0), 88)
        out.append(pred)
    return struct.pack("<%dh" % len(out), *out)


def _open_mono_wav(path: str) -> wave.Wave_write:
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SAMPLE_RATE)
    return w


# ---- 接收 / 统计 ----
class Receiver:
    def __init__(self, wav_path: str, fill_gaps: bool, split_nr: bool = True):
        self.wav_path = wav_path
        self.wav = _open_mono_wav(wav_path)
        self.fill_gaps = fill_gaps
        self.split_nr = split_nr
        stem, ext = os.path.splitext(wav_path)
        if not ext:
            ext = ".wav"
        self.bypass_path = f"{stem}_bypass{ext}"
        self.nr_path = f"{stem}_nr{ext}"
        self.wav_bypass = None
        self.wav_nr = None
        self.rx_bypass = self.rx_nr = 0
        self.last_nr = None
        self.expected = None
        self.last_pkt_type = None
        self.rx = self.lost = self.dup = self.bad = 0
        self.bytes = 0
        self.eof = False
        self.last_utc = None
        self.stop_evt = threading.Event()

    def _write_split(self, nr_on: bool, pcm: bytes, frames: int = 1):
        if not self.split_nr:
            return
        if nr_on:
            if self.wav_nr is None:
                self.wav_nr = _open_mono_wav(self.nr_path)
                print(f"[SPLIT] 检测到 NR ON，同步写入: {self.nr_path}")
            self.wav_nr.writeframes(pcm)
            self.rx_nr += frames
        else:
            if self.wav_bypass is None:
                self.wav_bypass = _open_mono_wav(self.bypass_path)
                print(f"[SPLIT] 检测到 Bypass，同步写入: {self.bypass_path}")
            self.wav_bypass.writeframes(pcm)
            self.rx_bypass += frames

    def on_sdu(self, sdu: bytes):
        if len(sdu) < HDR_SIZE:
            self.bad += 1
            return
        seq, pkt_type, codec, flags, utc, _ = struct.unpack_from(HDR_FMT, sdu, 0)
        body = sdu[HDR_SIZE:]
        if codec == 0 and len(body) == PCM_BYTES:
            pcm = body
        elif codec == 1 and len(body) == ADPCM_BYTES:
            pcm = adpcm_decode_frame(body)
        else:
            self.bad += 1
            print(f"[WARN] 非法帧 seq={seq} codec={codec} len={len(body)}")
            return
        self.last_utc = (utc, bool(flags & FLAG_UTC))
        if self.last_pkt_type is not None and pkt_type != self.last_pkt_type:
            print(f"[STREAM] pkt_type {self.last_pkt_type} -> {pkt_type} (重置 seq 期望)")
            self.expected = None
        elif self.expected is not None and seq == 0 and 0 < self.expected < 0xFFF0:
            # 非 65535 回绕的 seq=0 视为新会话/重传起点（如无 EOF 中断后重新采集或续传）
            print(f"[SYNC] seq 从 0 重新开始 (prev expected={self.expected})")
            self.expected = None
        self.last_pkt_type = pkt_type

        nr_on = bool(flags & FLAG_NR)
        if self.last_nr is not None and nr_on != self.last_nr:
            print(f"[MODE] 降噪切换: {'NR ON' if self.last_nr else 'Bypass'} -> "
                  f"{'NR ON' if nr_on else 'Bypass'} (seq={seq})")
        self.last_nr = nr_on

        if self.expected is not None:
            gap = (seq - self.expected) & 0xFFFF
            if gap == 0:
                pass
            elif gap < 0x8000:  # 前向跳变 -> 丢 gap 帧
                self.lost += gap
                print(f"[LOSS] seq {self.expected}..{(seq - 1) & 0xFFFF} 丢 {gap} 帧")
                if self.fill_gaps:
                    silence = b"\x00" * (PCM_BYTES * gap)
                    self.wav.writeframes(silence)
                    self._write_split(nr_on, silence, frames=gap)
            else:  # 重复/乱序, 丢弃
                self.dup += 1
                print(f"[DUP ] seq={seq} expected={self.expected}")
                return
        self.rx += 1
        self.bytes += len(sdu)
        self.expected = (seq + 1) & 0xFFFF
        self.wav.writeframes(pcm)
        self._write_split(nr_on, pcm, frames=1)
        if flags & FLAG_EOF:
            self.eof = True
            self.expected = None  # 下一个补传文件 seq 从 0 重新开始
            print(f"[EOF ] seq={seq} pkt_type={pkt_type}")

    def rx_loop(self, sock: socket.socket):
        sock.settimeout(0.5)
        while not self.stop_evt.is_set():
            try:
                sdu = sock.recv(2048)
            except socket.timeout:
                continue
            except OSError as e:
                print(f"[L2CAP] 接收结束: {e}")
                break
            if not sdu:
                print("[L2CAP] 对端关闭")
                break
            self.on_sdu(sdu)
        self.stop_evt.set()

    def summary(self):
        total = self.rx + self.lost
        pct = 100.0 * self.lost / total if total else 0.0
        print(f"\n=== 统计 === 收到 {self.rx} 帧 ({self.bytes} B), 丢帧 {self.lost} "
              f"({pct:.2f}%), 重复/乱序 {self.dup}, 非法 {self.bad}, "
              f"音频 {self.rx * 0.02:.2f} s")
        if self.split_nr and self.rx_bypass > 0 and self.rx_nr > 0:
            print(f"分轨保存: Bypass {self.rx_bypass} 帧 ({self.rx_bypass * 0.02:.2f} s) -> {self.bypass_path}")
            print(f"          NR ON  {self.rx_nr} 帧 ({self.rx_nr * 0.02:.2f} s) -> {self.nr_path}")

    def close(self):
        self.wav.close()
        if self.wav_bypass is not None:
            self.wav_bypass.close()
        if self.wav_nr is not None:
            self.wav_nr.close()
        # 仅出现单一模式时清理多余分轨副本，保留原输出文件即可
        if self.split_nr and not (self.rx_bypass > 0 and self.rx_nr > 0):
            for p in (self.bypass_path, self.nr_path):
                if os.path.exists(p):
                    try:
                        os.remove(p)
                    except OSError:
                        pass


def parse_status(data: bytes) -> str:
    if len(data) < 10:
        return f"status len={len(data)}"
    st, nr, codec, phy, used, pend, drop = struct.unpack("<BBBBBBI", data[:10])
    return (f"state={SYS_STATES.get(st, st)} nr={nr} codec={'ADPCM' if codec else 'PCM'} "
            f"phy={phy}M qspi={used}% pending={pend} drop={drop}")


async def find_device(name: str, address, timeout: float):
    if address:
        return await BleakScanner.find_device_by_address(address, timeout=timeout)
    return await BleakScanner.find_device_by_name(name, timeout=timeout)


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("-n", "--name", default=DEV_NAME)
    ap.add_argument("-a", "--address", help="直接指定 MAC (跳过按名称扫描)")
    ap.add_argument("-o", "--output", default="ble_audio.wav")
    ap.add_argument("--codec", choices=["pcm", "adpcm"], help="下发 0x06 设置编码")
    ap.add_argument("--nr", type=int, choices=[0, 1], help="下发 0x03 设置降噪")
    ap.add_argument("--duration", type=float, default=0, help="自动开始采集并持续 N 秒后停止 (0=交互模式)")
    ap.add_argument("--no-utc", action="store_true", help="不下发 0x05 校时")
    ap.add_argument("--fill-gaps", action="store_true", help="丢帧处写入静音以保持时间轴")
    ap.add_argument("--no-split-nr", action="store_true", help="禁用按 FLAG_NR 自动拆分 Bypass / NR ON 两个 WAV")
    ap.add_argument("--scan-timeout", type=float, default=15.0)
    args = ap.parse_args()

    print(f"[SCAN] 查找 {args.address or args.name} ...")
    dev = await find_device(args.name, args.address, args.scan_timeout)
    if dev is None:
        sys.exit("未找到设备")
    props = (getattr(dev, "details", None) or {}).get("props", {}) if isinstance(
        getattr(dev, "details", None), dict) else {}
    addr_type = BDADDR_LE_RANDOM if props.get("AddressType") == "random" else BDADDR_LE_PUBLIC
    print(f"[SCAN] 找到 {dev.name} {dev.address} ({'random' if addr_type == 2 else 'public'})")

    rcv = Receiver(args.output, args.fill_gaps, split_nr=not args.no_split_nr)
    loop = asyncio.get_running_loop()
    sock = None
    try:
        async with BleakClient(dev) as cli:
            print("[GATT] 已连接")
            # 0x1004 L2CAP Info
            info = bytes(await cli.read_gatt_char(UUID_INFO))
            psm, max_sdu = struct.unpack("<HH", info[:4])
            print(f"[GATT] 0x1004: PSM=0x{psm:04X} max_sdu={max_sdu}")

            async def ctrl(op, param=b""):
                await cli.write_gatt_char(UUID_CTRL, bytes([op]) + param, response=True)

            # 0x05 UTC 校时
            if not args.no_utc:
                await ctrl(OP_UTC, struct.pack("<I", int(time.time())))
                print("[CTRL] 0x05 UTC 校时已下发")

            # 订阅 Status
            def on_status(_, data):
                print("[STAT]", parse_status(bytes(data)))
            await cli.start_notify(UUID_STATUS, on_status)
            try:
                print("[STAT] 初值:", parse_status(bytes(await cli.read_gatt_char(UUID_STATUS))))
            except Exception:
                pass

            # L2CAP CoC (阻塞调用放线程)
            sock = await loop.run_in_executor(
                None, l2cap_coc_connect, dev.address, addr_type, psm, max(max_sdu, 650) + 64)
            print("[L2CAP] CoC 已建立")
            t = threading.Thread(target=rcv.rx_loop, args=(sock,), daemon=True)
            t.start()

            if args.codec:
                await ctrl(OP_CODEC, bytes([1 if args.codec == "adpcm" else 0]))
            if args.nr is not None:
                await ctrl(OP_NR, bytes([args.nr]))

            async def handle(cmd: str):
                p = cmd.split()
                if not p:
                    return True
                if p[0] == "start":
                    await ctrl(OP_START)
                elif p[0] == "stop":
                    await ctrl(OP_STOP)
                elif p[0] == "nr" and len(p) == 2:
                    await ctrl(OP_NR, bytes([int(p[1]) & 1]))
                elif p[0] == "codec" and len(p) == 2:
                    await ctrl(OP_CODEC, bytes([int(p[1]) & 1]))
                elif p[0] in ("quit", "q", "exit"):
                    return False
                else:
                    print("命令: start | stop | nr 0/1 | codec 0/1 | quit")
                return True

            if args.duration > 0:
                await ctrl(OP_START)
                print(f"[CTRL] 0x01 开始采集, {args.duration}s 后停止")
                end = loop.time() + args.duration
                while loop.time() < end and not rcv.stop_evt.is_set():
                    await asyncio.sleep(0.2)
                await ctrl(OP_STOP)
                await asyncio.sleep(1.5)  # 等待残余帧/EOF
            else:
                print("交互模式: start | stop | nr 0/1 | codec 0/1 | quit")
                while not rcv.stop_evt.is_set():
                    line = await loop.run_in_executor(None, sys.stdin.readline)
                    if not line or not await handle(line.strip()):
                        break
                try:
                    await ctrl(OP_STOP)
                except Exception:
                    pass
            await cli.stop_notify(UUID_STATUS)
    except KeyboardInterrupt:
        pass
    finally:
        rcv.stop_evt.set()
        if sock:
            sock.close()
        rcv.close()
        rcv.summary()
        print(f"WAV 已保存: {args.output}")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
