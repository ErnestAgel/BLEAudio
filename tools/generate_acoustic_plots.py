#!/usr/bin/env python3
"""Standardized acoustic & speech enhancement analyzer for BleAudio.

Evaluates PCM/WAV recordings using off-the-shelf industry-standard libraries and
pretrained models (no hand-rolled subjective/VAD thresholds):
  - Microsoft DNSMOS (ITU-T P.835 SIG/BAK/OVRL & P.808 MOS via `speechmos`)
  - Meta TorchAudio-SQUIM (`SQUIM_OBJECTIVE`: non-intrusive STOI, PESQ, SI-SDR)
  - ITU-R BS.1770 Integrated Loudness (LUFS via `torchaudio.functional.loudness`)
  - Silero VAD (`silero-vad` ONNX): automatic speech vs pure-noise segmentation
  - Welch PSD (`scipy.signal.welch`) & STFT Spectrogram (`librosa.display`)

Examples:
    python tools/generate_acoustic_plots.py sample/hw_test/test_pcm_bypass.wav sample/hw_test/test_pcm_nr.wav --labels "Bypass" "NR ON" -o compare.png
    python tools/generate_acoustic_plots.py capture.wav
    python tools/generate_acoustic_plots.py capture.wav --channel 1
    python tools/generate_acoustic_plots.py --baseline
"""

from __future__ import annotations

import argparse
import glob
import sys
import warnings
from pathlib import Path
from typing import Any

warnings.filterwarnings("ignore")

import librosa
import librosa.display
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy import signal
import soundfile as sf
from silero_vad import get_speech_timestamps, load_silero_vad
from speechmos import dnsmos
import torch
import torchaudio
from torchaudio.pipelines import SQUIM_OBJECTIVE

# Crisp CJK + ASCII rendering on Windows; fall back gracefully on other hosts
plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False
plt.rcParams["figure.dpi"] = 160

REPO_DIR = Path(__file__).resolve().parent.parent
SAMPLE_DIR = REPO_DIR / "sample"
ASSETS_DIR = SAMPLE_DIR / "assets"
PALETTE = ["#2563eb", "#d97706", "#dc2626", "#059669", "#7c3aed", "#0891b2"]
REF_CLEAN_WAV = SAMPLE_DIR / "dual_speaker" / "Set1_Office_Speaker1_Speech_16k.wav"
REF_NOISY_WAV = SAMPLE_DIR / "single_speaker" / "Single_Office_+5dB_16k.wav"
EVAL_SR = 16000
TARGET_LUFS = -25.0


class ModelBundle:
    """Lazy-loaded pretrained evaluation models shared across tracks."""

    def __init__(self) -> None:
        self._squim = None
        self._vad = None

    @property
    def squim(self):
        if self._squim is None:
            self._squim = SQUIM_OBJECTIVE.get_model()
        return self._squim

    @property
    def vad(self):
        if self._vad is None:
            self._vad = load_silero_vad(onnx=True)
        return self._vad


def load_wav(path: Path, channel: int = 0) -> tuple[int, np.ndarray, dict[str, Any]]:
    """Load a WAV file (8/16/24/32-bit PCM or float) as float32 in [-1.0, 1.0]."""
    if not path.is_file():
        raise FileNotFoundError(f"WAV file not found: {path}")
    info = sf.info(str(path))
    if info.samplerate <= 0:
        raise ValueError(f"Invalid sample rate ({info.samplerate} Hz) in {path}")
    if info.frames == 0:
        raise ValueError(f"WAV file has 0 frames: {path}")
    if channel < 0 or channel >= info.channels:
        raise ValueError(
            f"Requested channel {channel} out of range for {info.channels}-channel file: {path}"
        )

    data, sr = sf.read(str(path), dtype="float32", always_2d=True)
    x = data[:, channel].copy()
    clips = int(np.sum(np.abs(x) >= 0.9999))
    return int(sr), x, {
        "channels": info.channels,
        "selected_ch": channel,
        "subtype": info.subtype,
        "clips": clips,
    }


def _rms_db(x: np.ndarray) -> float:
    if len(x) == 0:
        return float("nan")
    return float(20.0 * np.log10(np.sqrt(np.mean(x.astype(np.float64) ** 2)) + 1e-12))


def _welch_db(x: np.ndarray, sr: int, nfft: int = 2048) -> tuple[np.ndarray, np.ndarray]:
    if len(x) < 16:
        return np.array([0.0, sr / 2.0]), np.array([-150.0, -150.0])
    nperseg = min(nfft, len(x))
    freqs, psd = signal.welch(x, fs=sr, window="hann", nperseg=nperseg, noverlap=nperseg // 2)
    return freqs, 10.0 * np.log10(psd + 1e-15)


def frame_rms_db(x: np.ndarray, sr: int, frame_ms: float = 20.0) -> tuple[np.ndarray, np.ndarray]:
    """Compute non-overlapping frame center times (s) and RMS levels (dBFS)."""
    hop = max(1, int(round(sr * frame_ms / 1000.0)))
    n = len(x) // hop
    if n == 0:
        return np.array([len(x) / (2.0 * sr)]), np.array([_rms_db(x)])
    frames = x[: n * hop].reshape(n, hop)
    rms = np.sqrt(np.mean(frames.astype(np.float64) ** 2, axis=1))
    t = (np.arange(n) + 0.5) * (hop / sr)
    return t, 20.0 * np.log10(rms + 1e-12)


def smoothed_frame_rms_db(
    x: np.ndarray, sr: int, frame_ms: float = 20.0, smooth_frames: int = 25
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Return frame times, raw frame RMS (dBFS), and moving-average envelope (dBFS)."""
    t, raw_db = frame_rms_db(x, sr, frame_ms)
    lin = 10.0 ** (raw_db / 10.0)
    k = min(smooth_frames, max(1, len(lin)))
    smooth_db = 10.0 * np.log10(np.convolve(lin, np.ones(k) / k, mode="same") + 1e-12)
    return t, raw_db, smooth_db


def analyze_track(label: str, path: Path, models: ModelBundle, channel: int = 0) -> dict[str, Any]:
    """Compute standardized objective, perceptual, loudness, and VAD-split metrics."""
    sr, x, meta = load_wav(path, channel=channel)
    t_native = torch.from_numpy(x).unsqueeze(0)
    dur = len(x) / sr

    # ITU-R BS.1770 loudness (fallback to NaN on ultra-short <0.4s clips)
    try:
        lufs = float(torchaudio.functional.loudness(t_native, sr).item())
    except Exception:
        lufs = float("nan")

    # Resample to 16 kHz for DNSMOS, SQUIM, and Silero VAD
    if sr != EVAL_SR:
        t16 = torchaudio.functional.resample(t_native, sr, EVAL_SR)
        x16 = np.clip(t16.squeeze(0).numpy(), -1.0, 1.0).astype(np.float32)
    else:
        t16 = t_native
        x16 = np.clip(x, -1.0, 1.0).astype(np.float32)

    # Meta SQUIM_OBJECTIVE (non-intrusive STOI, PESQ, SI-SDR)
    with torch.no_grad():
        stoi_t, pesq_t, sisdr_t = models.squim(t16)

    # Microsoft DNSMOS P.835 / P.808
    dmos = dnsmos.run(x16, sr=EVAL_SR, verbose=False)

    # Silero VAD speech vs pure-noise segmentation (with 150 ms edge guard for noise mask)
    ts = get_speech_timestamps(torch.from_numpy(x16), models.vad, sampling_rate=EVAL_SR, return_seconds=False)
    speech_mask_16 = np.zeros(len(x16), dtype=bool)
    noise_excl_16 = np.zeros(len(x16), dtype=bool)
    margin_16 = int(0.15 * EVAL_SR)
    for seg in ts:
        speech_mask_16[seg["start"] : seg["end"]] = True
        noise_excl_16[max(0, seg["start"] - margin_16) : min(len(x16), seg["end"] + margin_16)] = True
    noise_mask_16 = ~noise_excl_16

    vad_pct = float(100.0 * np.mean(speech_mask_16))
    sp_rms = _rms_db(x16[speech_mask_16]) if np.any(speech_mask_16) else float("nan")
    ns_rms = _rms_db(x16[noise_mask_16]) if np.any(noise_mask_16) else float("nan")
    contrast = (
        float(sp_rms - ns_rms) if not (np.isnan(sp_rms) or np.isnan(ns_rms)) else float("nan")
    )

    # 20 ms frame RMS split by VAD for boxplot distribution
    hop_16 = int(0.02 * EVAL_SR)
    n_fr = len(x16) // hop_16
    if n_fr > 0:
        fr_db = 20.0 * np.log10(
            np.sqrt(np.mean(x16[: n_fr * hop_16].reshape(n_fr, hop_16).astype(np.float64) ** 2, axis=1))
            + 1e-12
        )
        fr_sp_mask = speech_mask_16[: n_fr * hop_16].reshape(n_fr, hop_16).mean(axis=1) > 0.5
        fr_ns_mask = noise_mask_16[: n_fr * hop_16].reshape(n_fr, hop_16).mean(axis=1) > 0.5
        fr_db_sp = fr_db[fr_sp_mask]
        fr_db_ns = fr_db[fr_ns_mask]
    else:
        fr_db_sp = np.array([])
        fr_db_ns = np.array([])

    # Welch PSD (full, speech-only, pure-noise)
    f_all, psd_all_db = _welch_db(x, sr, nfft=2048)
    f_sp, psd_sp_db = (
        _welch_db(x16[speech_mask_16], EVAL_SR, nfft=1024)
        if np.sum(speech_mask_16) >= 32
        else (f_all, np.full_like(f_all, np.nan))
    )
    f_ns, psd_ns_db = (
        _welch_db(x16[noise_mask_16], EVAL_SR, nfft=1024)
        if np.sum(noise_mask_16) >= 32
        else (f_all, np.full_like(f_all, np.nan))
    )

    # Loudness-normalized copy (to -25 LUFS) for fair spectrogram & noise-floor comparison
    tot_rms = _rms_db(x)
    ref_lvl = lufs if not np.isnan(lufs) and np.isfinite(lufs) else tot_rms
    gain_db = TARGET_LUFS - ref_lvl
    x_norm = np.clip(x * (10.0 ** (gain_db / 20.0)), -1.0, 1.0).astype(np.float32)

    return {
        "label": label,
        "path": path,
        "sr": sr,
        "meta": meta,
        "dur": dur,
        "x": x,
        "x_norm": x_norm,
        "gain_db": float(gain_db),
        "tot_rms": tot_rms,
        "peak": float(20.0 * np.log10(np.max(np.abs(x)) + 1e-12)),
        "lufs": lufs,
        "sig": float(dmos["sig_mos"]),
        "bak": float(dmos["bak_mos"]),
        "ovrl": float(dmos["ovrl_mos"]),
        "p808": float(dmos["p808_mos"]),
        "stoi": float(stoi_t.item()),
        "pesq": float(pesq_t.item()),
        "sisdr": float(sisdr_t.item()),
        "ts_sec": [(s["start"] / EVAL_SR, s["end"] / EVAL_SR) for s in ts],
        "vad_pct": vad_pct,
        "sp_rms": sp_rms,
        "ns_rms": ns_rms,
        "contrast": contrast,
        "fr_db_sp": fr_db_sp,
        "fr_db_ns": fr_db_ns,
        "f_all": f_all,
        "psd_all_db": psd_all_db,
        "f_sp": f_sp,
        "psd_sp_db": psd_sp_db,
        "f_ns": f_ns,
        "psd_ns_db": psd_ns_db,
    }


def _fmt(v: float, spec: str = ".2f") -> str:
    return "  N/A" if np.isnan(v) or not np.isfinite(v) else format(v, spec)


def print_metrics_report(tracks: list[dict[str, Any]], show_delta: bool = False) -> None:
    """Print standardized evaluation metrics table and pairwise deltas vs first track."""
    w_lbl = max(24, max(len(r["label"]) for r in tracks))
    print("=== Standardized Acoustic & NR Evaluation Summary ===")
    print(
        f"{'Label':{w_lbl}s} | {'Fmt':9s} | {'Dur':>5s} | {'LUFS':>6s} | "
        f"{'SIG':>5s} {'BAK':>5s} {'OVRL':>5s} {'P808':>5s} | "
        f"{'STOI':>6s} {'PESQ':>5s} {'SI-SDR':>6s} | "
        f"{'VAD%':>5s} {'SpRMS':>6s} {'NsRMS':>6s} {'Sp-Ns':>6s}"
    )
    print("-" * (w_lbl + 108))
    for r in tracks:
        m = r["meta"]
        fmt_str = f"{m['channels']}ch/{r['sr']//1000:g}k"
        print(
            f"{r['label']:{w_lbl}s} | {fmt_str:9s} | {r['dur']:5.2f} | {_fmt(r['lufs'], '6.2f')} | "
            f"{r['sig']:5.2f} {r['bak']:5.2f} {r['ovrl']:5.2f} {r['p808']:5.2f} | "
            f"{r['stoi']:6.4f} {r['pesq']:5.2f} {r['sisdr']:6.2f} | "
            f"{r['vad_pct']:5.1f} {_fmt(r['sp_rms'], '6.2f')} {_fmt(r['ns_rms'], '6.2f')} {_fmt(r['contrast'], '6.2f')}"
        )

    if show_delta and len(tracks) >= 2:
        ref = tracks[0]
        for cur in tracks[1:]:
            print(f"\n=== Delta: {cur['label']} - {ref['label']} ===")
            deltas = [
                ("bak", "DNSMOS BAK (背景干净度)", "MOS"),
                ("sig", "DNSMOS SIG (人声保真度)", "MOS"),
                ("ovrl", "DNSMOS OVRL (综合听感)", "MOS"),
                ("p808", "DNSMOS P.808 (总体质量)", "MOS"),
                ("stoi", "SQUIM STOI (可懂度)", ""),
                ("pesq", "SQUIM PESQ (感知质量)", "MOS"),
                ("sisdr", "SQUIM SI-SDR (信噪/失真)", "dB"),
                ("ns_rms", "VAD 静音底噪 RMS", "dBFS"),
                ("sp_rms", "VAD 语音段 RMS", "dBFS"),
                ("contrast", "VAD 语噪对比度 (Sp-Ns)", "dB"),
                ("lufs", "ITU BS.1770 响度", "LUFS"),
            ]
            for key, desc, unit in deltas:
                v0, v1 = ref[key], cur[key]
                dv = v1 - v0 if not (np.isnan(v0) or np.isnan(v1)) else float("nan")
                print(f"  {desc:28s}: {_fmt(v0, '+7.3f')} -> {_fmt(v1, '+7.3f')}  (Delta = {_fmt(dv, '+7.3f')} {unit})")


def _bar_labels(ax: plt.Axes, bars, fmt: str = "{:.2f}", dy: float = 0.06, fontsize: float = 7.5) -> None:
    for b in bars:
        h = b.get_height()
        if np.isnan(h):
            continue
        ax.text(
            b.get_x() + b.get_width() / 2.0,
            h + (dy if h >= 0 else -3.2 * dy),
            fmt.format(h),
            ha="center",
            va="bottom",
            fontsize=fontsize,
            fontweight="bold",
        )


def plot_comparison_report(
    tracks: list[dict[str, Any]],
    refs: list[dict[str, Any]],
    out_path: Path,
    title: str,
) -> None:
    """Plot comprehensive visual diagnostic figure for 1 or more WAV files."""
    n = len(tracks)
    n_rows = (n + 2) if n >= 2 else 3
    fig_h = max(10.5, 3.1 * n_rows)
    fig = plt.figure(figsize=(17.0, fig_h), constrained_layout=True)
    height_ratios = [1.05] * max(n, 1) + ([1.1, 1.15] if n >= 2 else [1.1, 1.15])
    gs = fig.add_gridspec(n_rows, 3, height_ratios=height_ratios)
    fig.suptitle(title, fontsize=13.5, fontweight="bold")

    # 1. Per-track Waveform + Silero VAD & LUFS-normalized STFT Spectrogram
    max_amp = max(float(np.max(np.abs(d["x"]))) for d in tracks)
    w_ylim = max(0.05, min(1.05, 1.15 * max_amp))
    last_img = None
    spec_axes: list[plt.Axes] = []

    for i, d in enumerate(tracks):
        c = PALETTE[i % len(PALETTE)]
        ax_w = fig.add_subplot(gs[i, 0])
        t_ax = np.arange(len(d["x"])) / d["sr"]
        step = max(1, len(d["x"]) // 15000)
        if step > 1:
            nb = len(d["x"]) // step
            xb = d["x"][: nb * step].reshape(nb, step)
            tb = (np.arange(nb) + 0.5) * (step / d["sr"])
            ax_w.fill_between(tb, np.min(xb, axis=1), np.max(xb, axis=1), color=c, alpha=0.85, linewidth=0.3)
        else:
            ax_w.plot(t_ax, d["x"], color=c, linewidth=0.6)
        for j, (s, e) in enumerate(d["ts_sec"]):
            ax_w.axvspan(s, e, color="#22c55e", alpha=0.16, label="Silero VAD 语音段" if j == 0 else None)
        ax_w.set_xlim(0, d["dur"])
        ax_w.set_ylim(-w_ylim, w_ylim)
        ax_w.set_title(f"{d['label']} — 波形 & VAD (LUFS={_fmt(d['lufs'])})", fontsize=10.0, fontweight="bold", loc="left")
        ax_w.set_ylabel("幅度 (FS)")
        ax_w.set_xlabel("时间 (s)")
        ax_w.grid(True, linestyle=":", alpha=0.5)
        if d["ts_sec"]:
            ax_w.legend(loc="upper right", fontsize=8.0)

        ax_s = fig.add_subplot(gs[i, 1])
        spec_axes.append(ax_s)
        nfft_s = min(512, max(64, 2 ** int(np.floor(np.log2(max(64, len(d["x_norm"]) // 8))))))
        hop_s = max(16, nfft_s // 4)
        D = librosa.amplitude_to_db(np.abs(librosa.stft(d["x_norm"], n_fft=nfft_s, hop_length=hop_s)), ref=1.0)
        last_img = librosa.display.specshow(
            D, sr=d["sr"], hop_length=hop_s, x_axis="time", y_axis="hz", ax=ax_s, cmap="inferno", vmin=-65, vmax=5
        )
        for s, e in d["ts_sec"]:
            ax_s.axvline(s, color="#4ade80", linestyle="--", linewidth=0.65, alpha=0.6)
            ax_s.axvline(e, color="#f87171", linestyle=":", linewidth=0.65, alpha=0.6)
        ax_s.set_title(f"{d['label']} — 语谱图 (对齐至 {TARGET_LUFS:g} LUFS)", fontsize=10.0, fontweight="bold", loc="left")
        ax_s.set_xlabel("时间 (s)")
        ax_s.set_ylabel("频率 (Hz)")

    if last_img is not None and spec_axes:
        cbar = fig.colorbar(last_img, ax=spec_axes, fraction=0.025, pad=0.02)
        cbar.set_label("能量 (dB)", fontsize=9)

    # 2. Right column top: 20ms frame RMS boxplot + VAD level summary
    if n == 1:
        ax_box = fig.add_subplot(gs[0, 2])
        ax_vad = fig.add_subplot(gs[1, 2])
    elif n == 2:
        ax_box = fig.add_subplot(gs[0, 2])
        ax_vad = fig.add_subplot(gs[1, 2])
    else:
        ax_box = fig.add_subplot(gs[0 : n // 2, 2])
        ax_vad = fig.add_subplot(gs[n // 2 : n, 2])

    box_data, box_labels, box_colors = [], [], []
    for i, d in enumerate(tracks):
        c = PALETTE[i % len(PALETTE)]
        if len(d["fr_db_sp"]) > 0:
            box_data.append(d["fr_db_sp"])
            box_labels.append(f"{d['label'][:10]}\n语音帧")
            box_colors.append(c)
        if len(d["fr_db_ns"]) > 0:
            box_data.append(d["fr_db_ns"])
            box_labels.append(f"{d['label'][:10]}\n静音帧")
            box_colors.append(c)
    if box_data:
        bp = ax_box.boxplot(box_data, tick_labels=box_labels, patch_artist=True, showfliers=False)
        for patch, c in zip(bp["boxes"], box_colors):
            patch.set_facecolor(c)
            patch.set_alpha(0.55)
    ax_box.set_title("20 ms 帧能量分布 (Silero VAD 拆段)", fontsize=10.0, fontweight="bold", loc="left")
    ax_box.set_ylabel("帧 RMS (dBFS)")
    ax_box.grid(True, axis="y", linestyle=":", alpha=0.5)

    cats = ["语音段 RMS\n(SpRMS)", "静音底噪 RMS\n(NsRMS)", "语噪对比度\n(Sp-Ns, dB)"]
    x_idx = np.arange(len(cats))
    w_bar = min(0.35, 0.78 / max(1, n))
    for i, d in enumerate(tracks):
        vals = [d["sp_rms"], d["ns_rms"], d["contrast"]]
        offset = (i - (n - 1) / 2.0) * w_bar
        bars = ax_vad.bar(x_idx + offset, vals, w_bar, label=d["label"], color=PALETTE[i % len(PALETTE)], alpha=0.85)
        _bar_labels(ax_vad, bars, "{:.1f}", dy=1.2, fontsize=8.0)
    ax_vad.axhline(0, color="#475569", linewidth=0.8)
    ax_vad.set_xticks(x_idx)
    ax_vad.set_xticklabels(cats)
    ax_vad.set_ylim(-70, 32)
    ax_vad.set_title("VAD 分段电平与语噪对比度", fontsize=10.0, fontweight="bold", loc="left")
    ax_vad.set_ylabel("dBFS / dB")
    ax_vad.legend(loc="upper left", ncol=min(2, n), fontsize=8.0)
    ax_vad.grid(True, axis="y", linestyle=":", alpha=0.5)

    # 3. PSD Row (Noise-only PSD | Speech-only PSD | Loudness-calibrated Noise-only PSD)
    psd_row = 1 if n == 1 else n
    ax_pns = fig.add_subplot(gs[psd_row, 0])
    ax_psp = fig.add_subplot(gs[psd_row, 1])
    if n >= 2:
        ax_pnorm = fig.add_subplot(gs[psd_row, 2])
    else:
        ax_pnorm = None

    for i, d in enumerate(tracks):
        c = PALETTE[i % len(PALETTE)]
        if not np.all(np.isnan(d["psd_ns_db"])):
            ax_pns.semilogx(d["f_ns"][1:], d["psd_ns_db"][1:], color=c, linewidth=1.6, label=f"{d['label']} 静音段")
            if ax_pnorm is not None:
                ax_pnorm.semilogx(
                    d["f_ns"][1:],
                    d["psd_ns_db"][1:] + d["gain_db"],
                    color=c,
                    linewidth=1.6,
                    label=f"{d['label']} (对齐 {TARGET_LUFS:g} LUFS)",
                )
        if not np.all(np.isnan(d["psd_sp_db"])):
            ax_psp.semilogx(d["f_sp"][1:], d["psd_sp_db"][1:], color=c, linewidth=1.6, label=f"{d['label']} 语音段")
        else:
            ax_psp.semilogx(d["f_all"][1:], d["psd_all_db"][1:], color=c, linewidth=1.4, linestyle="--", label=f"{d['label']} 全段")

    ax_pns.axvline(100, color="#0284c7", linestyle="--", linewidth=1.0, label="100 Hz")
    ax_pns.set_xlim(40, 8000)
    ax_pns.set_title("纯静音/停顿段 Welch PSD (实测底噪谱)", fontsize=10.0, fontweight="bold", loc="left")
    ax_pns.set_xlabel("频率 (Hz)")
    ax_pns.set_ylabel("PSD (dBFS/Hz)")
    ax_pns.grid(True, which="both", linestyle=":", alpha=0.5)
    ax_pns.legend(loc="lower left", fontsize=8.0)

    ax_psp.axvspan(300, 4000, color="#cbd5e1", alpha=0.25, label="语音主频带 (0.3-4 kHz)")
    ax_psp.set_xlim(40, 8000)
    ax_psp.set_title("活跃语音段 Welch PSD (语音能量谱)", fontsize=10.0, fontweight="bold", loc="left")
    ax_psp.set_xlabel("频率 (Hz)")
    ax_psp.set_ylabel("PSD (dBFS/Hz)")
    ax_psp.grid(True, which="both", linestyle=":", alpha=0.5)
    ax_psp.legend(loc="lower left", fontsize=8.0)

    if ax_pnorm is not None:
        ax_pnorm.axvline(100, color="#0284c7", linestyle="--", linewidth=1.0, label="100 Hz")
        ax_pnorm.set_xlim(40, 8000)
        ax_pnorm.set_title("同响度校准后静音段 PSD (消除录音音量差)", fontsize=10.0, fontweight="bold", loc="left")
        ax_pnorm.set_xlabel("频率 (Hz)")
        ax_pnorm.set_ylabel("校准 PSD (dB/Hz)")
        ax_pnorm.grid(True, which="both", linestyle=":", alpha=0.5)
        ax_pnorm.legend(loc="lower left", fontsize=8.0)

    # 4. Bottom Row: DNSMOS + SQUIM STOI/PESQ + SQUIM SI-SDR (with optional Clean/Noisy refs)
    metric_row = n_rows - 1
    all_items = [(d["label"], d, PALETTE[i % len(PALETTE)], True) for i, d in enumerate(tracks)]
    ref_colors = ["#059669", "#64748b"]
    for j, r in enumerate(refs):
        all_items.append((r["label"], r, ref_colors[j % len(ref_colors)], False))
    m_cnt = len(all_items)
    w_m = min(0.24, 0.80 / max(1, m_cnt))

    ax_dns = fig.add_subplot(gs[metric_row, 0])
    dns_keys = ["sig", "bak", "ovrl", "p808"]
    dns_lbls = ["SIG\n(人声保真)", "BAK\n(背景干净度)", "OVRL\n(综合听感)", "P.808\n(总体 MOS)"]
    x_d = np.arange(len(dns_keys))
    for i, (lbl, d, c, is_user) in enumerate(all_items):
        offset = (i - (m_cnt - 1) / 2.0) * w_m
        bars = ax_dns.bar(x_d + offset, [d[k] for k in dns_keys], w_m, label=lbl, color=c, alpha=0.85)
        if is_user:
            _bar_labels(ax_dns, bars, "{:.2f}", dy=0.06, fontsize=7.5)
    ax_dns.set_xticks(x_d)
    ax_dns.set_xticklabels(dns_lbls)
    ax_dns.set_ylim(1.0, 5.0)
    ax_dns.set_title("Microsoft DNSMOS (ITU-T P.835 / P.808, 1-5 分)", fontsize=10.0, fontweight="bold", loc="left")
    ax_dns.set_ylabel("MOS 评分 (越高越好)")
    ax_dns.legend(loc="upper right", ncol=2, fontsize=7.5)
    ax_dns.grid(True, axis="y", linestyle=":", alpha=0.5)

    ax_sq1 = fig.add_subplot(gs[metric_row, 1])
    x_s = np.arange(2)
    for i, (lbl, d, c, is_user) in enumerate(all_items):
        offset = (i - (m_cnt - 1) / 2.0) * w_m
        vals = [d["stoi"] * 5.0, d["pesq"]]
        bars = ax_sq1.bar(x_s + offset, vals, w_m, label=lbl, color=c, alpha=0.85)
        if is_user:
            raw_strs = [f"{d['stoi']:.3f}", f"{d['pesq']:.2f}"]
            for b, rs in zip(bars, raw_strs):
                ax_sq1.text(b.get_x() + b.get_width() / 2.0, b.get_height() + 0.08, rs, ha="center", va="bottom", fontsize=7.8, fontweight="bold")
    ax_sq1.set_xticks(x_s)
    ax_sq1.set_xticklabels(["STOI (×5 缩放显示)\n[0-1 可懂度]", "PESQ\n[-0.5 ~ 4.5 质量]"])
    ax_sq1.set_ylim(0, 5.6)
    ax_sq1.set_title("Meta SQUIM — 客观可懂度与感知质量", fontsize=10.0, fontweight="bold", loc="left")
    ax_sq1.set_ylabel("分值")
    ax_sq1.legend(loc="upper right", ncol=2, fontsize=7.5)
    ax_sq1.grid(True, axis="y", linestyle=":", alpha=0.5)

    ax_sq2 = fig.add_subplot(gs[metric_row, 2])
    x_r = np.arange(m_cnt)
    vals_r = [d["sisdr"] for _, d, _, _ in all_items]
    bars = ax_sq2.bar(x_r, vals_r, width=0.52, color=[c for _, _, c, _ in all_items], alpha=0.85)
    _bar_labels(ax_sq2, bars, "{:.2f} dB", dy=0.5, fontsize=8.0)
    ax_sq2.set_xticks(x_r)
    ax_sq2.set_xticklabels([lbl for lbl, _, _, _ in all_items], rotation=12 if m_cnt > 4 else 0)
    top_sdr = max([v for v in vals_r if not np.isnan(v)] + [20.0]) + 4.5
    bot_sdr = min([v for v in vals_r if not np.isnan(v)] + [0.0]) - 2.0
    ax_sq2.set_ylim(min(0.0, bot_sdr), top_sdr)
    ax_sq2.set_title("Meta SQUIM — 预测 SI-SDR (信噪与失真比)", fontsize=10.0, fontweight="bold", loc="left")
    ax_sq2.set_ylabel("SI-SDR (dB, 越高越好)")
    ax_sq2.grid(True, axis="y", linestyle=":", alpha=0.5)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, bbox_inches="tight")
    plt.close(fig)


def plot_baseline_suite(title: str, tracks_cfg: list[tuple[str, Path, str]], out_path: Path) -> None:
    """Render the 3-row baseline reference figure (Waveform | Spectrogram | Welch PSD) for sample/assets/."""
    fig = plt.figure(figsize=(16.5, 9.5), constrained_layout=True)
    gs = fig.add_gridspec(len(tracks_cfg), 3, width_ratios=[1.25, 1.45, 1.05], hspace=0.22)
    fig.suptitle(title, fontsize=13.5, fontweight="bold")
    ax_psd = fig.add_subplot(gs[:, 2])
    spec_axes: list[plt.Axes] = []
    last_img = None

    for row, (label, wav_path, color) in enumerate(tracks_cfg):
        sr, x, _ = load_wav(wav_path)
        dur = len(x) / sr

        ax_w = fig.add_subplot(gs[row, 0])
        step = max(1, len(x) // 15000)
        nb = len(x) // step
        xb = x[: nb * step].reshape(nb, step)
        tb = (np.arange(nb) + 0.5) * (step / sr)
        ax_w.fill_between(tb, np.min(xb, axis=1), np.max(xb, axis=1), color=color, alpha=0.85, linewidth=0.3)
        ax_w.axvspan(0.0, 2.0, color="#94a3b8", alpha=0.22, label="0-2 s 前导参考" if row == 0 else None)
        ax_w.axvline(2.0, color="#475569", linestyle="--", linewidth=0.9)
        if row == 0:
            ax_w.legend(loc="upper right", fontsize=8, framealpha=0.9)
        ax_w.set_xlim(0, 30)
        ax_w.set_ylim(-0.85, 0.85)
        ax_w.set_ylabel("幅度 (FS)", fontsize=9)
        ax_w.set_title(f"{label} — 时域波形", fontsize=10, fontweight="bold", loc="left")
        ax_w.grid(True, linestyle=":", alpha=0.5)
        if row == len(tracks_cfg) - 1:
            ax_w.set_xlabel("时间 (s)", fontsize=9)

        ax_s = fig.add_subplot(gs[row, 1])
        spec_axes.append(ax_s)
        D = librosa.amplitude_to_db(np.abs(librosa.stft(x, n_fft=512, hop_length=128)), ref=1.0)
        last_img = librosa.display.specshow(
            D, sr=sr, hop_length=128, x_axis="time", y_axis="hz", ax=ax_s, cmap="inferno", vmin=-95, vmax=-25
        )
        ax_s.axhline(100.0, color="#38bdf8", linestyle="--", linewidth=0.9, alpha=0.9)
        ax_s.axvline(2.0, color="#e2e8f0", linestyle="--", linewidth=0.8, alpha=0.75)
        ax_s.set_xlim(0, 30)
        ax_s.set_ylim(0, sr / 2.0)
        ax_s.set_ylabel("频率 (Hz)", fontsize=9)
        ax_s.set_title(f"{label} — STFT 语谱图 ({sr / 1000:g} kHz)", fontsize=10, fontweight="bold", loc="left")
        if row == len(tracks_cfg) - 1:
            ax_s.set_xlabel("时间 (s)", fontsize=9)

        freqs_p, psd_db = _welch_db(x, sr, nfft=2048)
        mask = freqs_p >= 30.0
        ax_psd.semilogx(freqs_p[mask], psd_db[mask], color=color, linewidth=1.7, label=label)

    if last_img is not None and spec_axes:
        cbar = fig.colorbar(last_img, ax=spec_axes, fraction=0.025, pad=0.02)
        cbar.set_label("能量密度 (dBFS)", fontsize=9)

    ax_psd.axvline(100.0, color="#0284c7", linestyle="--", linewidth=1.1, label="100 Hz HPF 截止线")
    ax_psd.axvspan(300.0, 4000.0, color="#cbd5e1", alpha=0.22, label="语音主能量带 (0.3-4 kHz)")
    ax_psd.set_xlim(30.0, 8000.0)
    ax_psd.set_ylim(-105.0, -35.0)
    ax_psd.set_xlabel("频率 (Hz, 对数坐标)", fontsize=9)
    ax_psd.set_ylabel("功率谱密度 (dBFS/Hz)", fontsize=9)
    ax_psd.set_title("Welch 平均功率谱对比 (LTASS / PSD)", fontsize=10, fontweight="bold")
    ax_psd.grid(True, which="both", linestyle=":", alpha=0.5)
    ax_psd.legend(loc="lower left", fontsize=8, framealpha=0.92)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, bbox_inches="tight")
    plt.close(fig)


def plot_envelope_and_snr(out_path: Path) -> None:
    """Plot 20ms frame RMS trajectories + 0.5s smoothed contours for Set 1 and Set 2 baselines."""
    fig, axes = plt.subplots(2, 1, figsize=(13.5, 7.5), sharex=True, constrained_layout=True)
    fig.suptitle("BleAudio 测试基准短时能量包络与动态 SNR 分布 (20 ms 帧 RMS)", fontsize=13, fontweight="bold")

    configs = [
        (
            axes[0],
            "Set 1 — 稳态办公室场景 (Office): +5 dB 常规对照",
            [
                ("Speaker 1 纯人声 (Set1_Office_Speaker1_Speech)", SAMPLE_DIR / "dual_speaker" / "Set1_Office_Speaker1_Speech_16k.wav", PALETTE[0]),
                ("Speaker 2 纯噪声 (Set1_Office_Speaker2_Noise)", SAMPLE_DIR / "dual_speaker" / "Set1_Office_Speaker2_Noise_16k.wav", PALETTE[1]),
                ("单音响混音 (Single_Office_+5dB)", SAMPLE_DIR / "single_speaker" / "Single_Office_+5dB_16k.wav", PALETTE[2]),
            ],
        ),
        (
            axes[1],
            "Set 2 — 动态地铁场景 (Metro): 0 dB 严苛对照",
            [
                ("Speaker 1 纯人声 (Set2_Metro_Speaker1_Speech)", SAMPLE_DIR / "dual_speaker" / "Set2_Metro_Speaker1_Speech_16k.wav", PALETTE[0]),
                ("Speaker 2 纯噪声 (Set2_Metro_Speaker2_Noise)", SAMPLE_DIR / "dual_speaker" / "Set2_Metro_Speaker2_Noise_16k.wav", PALETTE[1]),
                ("单音响混音 (Single_Metro_0dB)", SAMPLE_DIR / "single_speaker" / "Single_Metro_0dB_16k.wav", PALETTE[2]),
            ],
        ),
    ]

    for ax, subtitle, items in configs:
        for label, p, color in items:
            sr, x, _ = load_wav(p)
            t, fdb, smooth_db = smoothed_frame_rms_db(x, sr, 20.0, 25)
            ax.plot(t, np.clip(fdb, -75, 0), color=color, alpha=0.28, linewidth=0.7)
            ax.plot(t, np.clip(smooth_db, -75, 0), color=color, linewidth=1.8, label=label)

        ax.axvspan(0.0, 2.0, color="#94a3b8", alpha=0.20, label="0-2 s 纯噪声收敛前导")
        ax.axvline(2.0, color="#475569", linestyle="--", linewidth=1.0)
        ax.set_xlim(0, 30)
        ax.set_ylim(-65, -10)
        ax.set_ylabel("帧能量 (dBFS)", fontsize=9.5)
        ax.set_title(subtitle, fontsize=10.5, fontweight="bold", loc="left")
        ax.grid(True, linestyle=":", alpha=0.55)
        ax.legend(loc="lower right", ncol=2, fontsize=8.2, framealpha=0.92)

    axes[1].set_xlabel("时间 (s)", fontsize=10)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, bbox_inches="tight")
    plt.close(fig)


def resolve_wav_args(raw_args: list[str]) -> list[Path]:
    """Expand wildcards (useful in Windows PowerShell) and validate WAV inputs."""
    resolved: list[Path] = []
    for item in raw_args:
        if any(c in item for c in "*?[]"):
            matches = sorted(Path(m).resolve() for m in glob.glob(item))
            if not matches:
                raise FileNotFoundError(f"No files matched pattern: {item}")
            resolved.extend(matches)
        else:
            resolved.append(Path(item).resolve())
    return resolved


def run_baseline(models: ModelBundle) -> None:
    """Regenerate the 3 audio-derived baseline figures in sample/assets/ and report metrics."""
    set1_tracks = [
        ("Speaker 1 纯人声 (美式英语 F1/M1/F2/M2)", SAMPLE_DIR / "dual_speaker" / "Set1_Office_Speaker1_Speech_16k.wav", PALETTE[0]),
        ("Speaker 2 纯噪声 (稳态办公室)", SAMPLE_DIR / "dual_speaker" / "Set1_Office_Speaker2_Noise_16k.wav", PALETTE[1]),
        ("单音响混音 (+5 dB SNR)", SAMPLE_DIR / "single_speaker" / "Single_Office_+5dB_16k.wav", PALETTE[2]),
    ]
    set2_tracks = [
        ("Speaker 1 纯人声 (英式英语 F1/M1/F2/M2)", SAMPLE_DIR / "dual_speaker" / "Set2_Metro_Speaker1_Speech_16k.wav", PALETTE[0]),
        ("Speaker 2 纯噪声 (动态地铁)", SAMPLE_DIR / "dual_speaker" / "Set2_Metro_Speaker2_Noise_16k.wav", PALETTE[1]),
        ("单音响混音 (0 dB SNR)", SAMPLE_DIR / "single_speaker" / "Single_Metro_0dB_16k.wav", PALETTE[2]),
    ]

    plot_baseline_suite(
        "Set 1 稳态办公室场景 (Office) — 声学基线图谱 (16 kHz / 16-bit Mono)",
        set1_tracks,
        ASSETS_DIR / "set1_office_baseline.png",
    )
    plot_baseline_suite(
        "Set 2 动态地铁场景 (Metro) — 声学基线图谱 (16 kHz / 16-bit Mono)",
        set2_tracks,
        ASSETS_DIR / "set2_metro_baseline.png",
    )
    plot_envelope_and_snr(ASSETS_DIR / "snr_envelope_comparison.png")

    all_wavs = sorted(
        list((SAMPLE_DIR / "dual_speaker").glob("*.wav"))
        + list((SAMPLE_DIR / "single_speaker").glob("*.wav"))
    )
    results = [analyze_track(w.relative_to(SAMPLE_DIR).as_posix(), w, models) for w in all_wavs]
    print_metrics_report(results, show_delta=False)


def run_custom(args: argparse.Namespace, models: ModelBundle) -> None:
    """Analyze 1 or more arbitrary WAV files and export a unified diagnostic PNG."""
    wav_paths = resolve_wav_args(args.wavs)
    labels = args.labels if args.labels else [p.stem for p in wav_paths]
    if len(labels) != len(wav_paths):
        raise ValueError(f"--labels count ({len(labels)}) must match number of WAV files ({len(wav_paths)}).")

    tracks = [analyze_track(lbl, p, models, channel=args.channel) for lbl, p in zip(labels, wav_paths)]
    refs: list[dict[str, Any]] = []
    if not args.no_ref and REF_CLEAN_WAV.is_file() and REF_NOISY_WAV.is_file():
        refs.append(analyze_track("基线: Set1 纯人声", REF_CLEAN_WAV, models))
        refs.append(analyze_track("基线: Office +5dB", REF_NOISY_WAV, models))

    if args.out:
        out_path = Path(args.out).resolve()
    elif len(wav_paths) == 1:
        out_path = wav_paths[0].with_name(f"{wav_paths[0].stem}_acoustic_analysis.png")
    else:
        out_path = wav_paths[0].parent / f"{wav_paths[0].stem}_vs_{wav_paths[1].stem}_analysis.png"

    title = args.title or (
        f"BleAudio 音频标准评估报告 — {labels[0]}"
        if len(tracks) == 1
        else f"BleAudio 降噪对比评估报告 — {' vs '.join(labels)} (DNSMOS + SQUIM + Silero VAD + LUFS)"
    )
    plot_comparison_report(tracks, refs, out_path, title)
    print(f"Saved acoustic plot: {out_path}")
    print_metrics_report(tracks, show_delta=True)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Standardized WAV acoustic & NR evaluation plotter (DNSMOS + SQUIM + Silero VAD + LUFS)."
    )
    parser.add_argument("wavs", nargs="*", help="One or more WAV files (supports wildcards) to analyze/compare.")
    parser.add_argument("--baseline", action="store_true", help="Regenerate baseline figures in sample/assets/ and print metrics.")
    parser.add_argument("-c", "--channel", type=int, default=0, help="Channel index for multi-channel WAVs (default: 0).")
    parser.add_argument("--labels", nargs="+", help="Custom legend labels matching input WAV files.")
    parser.add_argument("-o", "--out", type=str, help="Output PNG path for custom WAV analysis.")
    parser.add_argument("--title", type=str, help="Custom figure title.")
    parser.add_argument("--no-ref", action="store_true", help="Omit Set1 clean/noisy baseline bars in metric charts.")
    args = parser.parse_args()

    models = ModelBundle()
    try:
        if args.baseline or not args.wavs:
            run_baseline(models)
        else:
            run_custom(args, models)
    except (FileNotFoundError, ValueError, RuntimeError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
