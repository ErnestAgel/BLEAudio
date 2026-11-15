#!/usr/bin/env python3
"""General-purpose PCM WAV acoustic analyzer and diagnostic plotter for BleAudio.

Supports any uncompressed 8/16/32-bit mono or multi-channel PCM WAV file of any
sample rate and duration, producing synchronized Time-Domain Waveform, STFT
Spectrogram, Welch PSD, and 20 ms Frame RMS Envelope plots alongside quantitative
metrics (and pairwise dB deltas when comparing 2+ files such as Bypass vs NR ON).

Examples:
    python sample/generate_acoustic_plots.py capture.wav
    python sample/generate_acoustic_plots.py capture.wav --channel 1
    python sample/generate_acoustic_plots.py bypass.wav nr_on.wav --labels "Bypass" "NR ON" -o compare.png
    python sample/generate_acoustic_plots.py --baseline
"""

from __future__ import annotations

import argparse
import glob
import sys
import wave
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# Crisp CJK + ASCII rendering on Windows; fall back gracefully on other hosts
plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False
plt.rcParams["figure.dpi"] = 160

SAMPLE_DIR = Path(__file__).resolve().parent
ASSETS_DIR = SAMPLE_DIR / "assets"
PALETTE = ["#2563eb", "#d97706", "#dc2626", "#059669", "#7c3aed", "#0891b2"]


def load_wav(path: Path, channel: int = 0) -> tuple[int, np.ndarray, dict[str, int]]:
    """Load an uncompressed PCM WAV file (8/16/32-bit) as float64 in [-1.0, 1.0)."""
    if not path.is_file():
        raise FileNotFoundError(f"WAV file not found: {path}")
    with wave.open(str(path), "rb") as w:
        ch = w.getnchannels()
        sw = w.getsampwidth()
        sr = w.getframerate()
        nf = w.getnframes()
        raw = w.readframes(nf)

    if sr <= 0:
        raise ValueError(f"Invalid sample rate ({sr} Hz) in {path}")
    if nf == 0:
        raise ValueError(f"WAV file has 0 frames: {path}")
    if channel < 0 or channel >= ch:
        raise ValueError(f"Requested channel {channel} out of range for {ch}-channel file: {path}")

    if sw == 1:
        raw_u8 = np.frombuffer(raw, dtype=np.uint8)
        if ch > 1:
            raw_u8 = raw_u8.reshape(-1, ch)[:, channel]
        arr = (raw_u8.astype(np.float64) - 128.0) / 128.0
        clips = int(np.sum((raw_u8 == 0) | (raw_u8 == 255)))
    elif sw == 2:
        raw_i16 = np.frombuffer(raw, dtype=np.int16)
        if ch > 1:
            raw_i16 = raw_i16.reshape(-1, ch)[:, channel]
        arr = raw_i16.astype(np.float64) / 32768.0
        clips = int(np.sum((raw_i16 == -32768) | (raw_i16 == 32767)))
    elif sw == 4:
        raw_i32 = np.frombuffer(raw, dtype=np.int32)
        if ch > 1:
            raw_i32 = raw_i32.reshape(-1, ch)[:, channel]
        arr = raw_i32.astype(np.float64) / 2147483648.0
        clips = int(np.sum((raw_i32 <= -2147483647) | (raw_i32 >= 2147483647)))
    else:
        raise ValueError(f"Unsupported PCM bit depth ({8 * sw}-bit) in {path}; expected 8/16/32-bit.")

    return sr, arr, {"channels": ch, "selected_ch": channel, "bits": 8 * sw, "clips": clips}


def frame_rms_db(x: np.ndarray, sr: int, frame_ms: float = 20.0) -> tuple[np.ndarray, np.ndarray]:
    """Compute non-overlapping frame center times (s) and RMS levels (dBFS)."""
    hop = max(1, int(round(sr * frame_ms / 1000.0)))
    n = len(x) // hop
    if n == 0:
        rms = np.array([float(np.sqrt(np.mean(x ** 2)))])
        t = np.array([len(x) / (2.0 * sr)])
        return t, 20.0 * np.log10(rms + 1e-12)
    frames = x[: n * hop].reshape(n, hop)
    rms = np.sqrt(np.mean(frames ** 2, axis=1))
    t = (np.arange(n) + 0.5) * (hop / sr)
    return t, 20.0 * np.log10(rms + 1e-12)


def smoothed_frame_rms_db(
    x: np.ndarray, sr: int, frame_ms: float = 20.0, smooth_frames: int = 25
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Return frame times, raw frame RMS (dBFS), and linear-power moving-average envelope (dBFS)."""
    t, raw_db = frame_rms_db(x, sr, frame_ms)
    lin = 10.0 ** (raw_db / 10.0)
    k = min(smooth_frames, max(1, len(lin)))
    smooth_db = 10.0 * np.log10(np.convolve(lin, np.ones(k) / k, mode="same") + 1e-12)
    return t, raw_db, smooth_db


def welch_psd_db(x: np.ndarray, sr: int, nfft: int = 2048) -> tuple[np.ndarray, np.ndarray]:
    """Compute Welch average power spectral density in dBFS/Hz (Hann window, 50% overlap)."""
    nfft_eff = min(nfft, max(64, 2 ** int(np.floor(np.log2(max(64, len(x)))))))
    if len(x) < nfft_eff:
        x = np.pad(x, (0, nfft_eff - len(x)))
    hop = max(1, nfft_eff // 2)
    win = np.hanning(nfft_eff)
    win_norm = np.sum(win ** 2) * sr
    n_frames = max(1, (len(x) - nfft_eff) // hop + 1)
    acc = np.zeros(nfft_eff // 2 + 1, dtype=np.float64)
    for i in range(n_frames):
        seg = x[i * hop : i * hop + nfft_eff]
        mag2 = np.abs(np.fft.rfft(seg * win)) ** 2
        if nfft_eff % 2 == 0:
            mag2[1:-1] *= 2.0
        else:
            mag2[1:] *= 2.0
        acc += mag2 / win_norm
    acc /= n_frames
    freqs = np.fft.rfftfreq(nfft_eff, d=1.0 / sr)
    return freqs, 10.0 * np.log10(acc + 1e-15)


def compute_metrics(path: Path, channel: int = 0) -> dict[str, float]:
    """Compute objective acoustic metrics for any PCM WAV file."""
    sr, x, meta = load_wav(path, channel=channel)
    dur = len(x) / sr
    dc = float(np.mean(x))
    tot_rms = float(20.0 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12))
    peak = float(20.0 * np.log10(np.max(np.abs(x)) + 1e-12))
    crest = 0.0 if peak <= -200.0 else (peak - tot_rms)

    _, fdb = frame_rms_db(x, sr, 20.0)
    act_frames = fdb[fdb > -40.0]
    act_rms = (
        float(10.0 * np.log10(np.mean(10.0 ** (act_frames / 10.0)) + 1e-12))
        if len(act_frames)
        else tot_rms
    )
    noise_p10 = float(np.percentile(fdb, 10))
    lead_rms = float(20.0 * np.log10(np.sqrt(np.mean(x[: min(len(x), 2 * sr)] ** 2)) + 1e-12))

    freqs, psd_db = welch_psd_db(x, sr, 2048)
    psd_lin = 10.0 ** (psd_db / 10.0)
    df = freqs[1] - freqs[0] if len(freqs) > 1 else 1.0
    total_pow = float(np.sum(psd_lin) * df) + 1e-15
    sub100_pct = float(100.0 * np.sum(psd_lin[freqs < 100.0]) * df / total_pow)
    voice_pct = float(100.0 * np.sum(psd_lin[(freqs >= 300.0) & (freqs <= min(4000.0, 0.5 * sr))]) * df / total_pow)

    return {
        "sr": float(sr),
        "ch": float(meta["channels"]),
        "bits": float(meta["bits"]),
        "clips": float(meta["clips"]),
        "dur": dur,
        "dc": dc,
        "tot_rms": tot_rms,
        "act_rms": act_rms,
        "noise_p10": noise_p10,
        "peak": peak,
        "crest": crest,
        "lead_rms": lead_rms,
        "sub100_pct": sub100_pct,
        "voice_pct": voice_pct,
    }


def plot_tracks_diagnostic(
    title: str,
    tracks: list[tuple[str, Path, str]],
    out_path: Path,
    channel: int = 0,
    fixed_baseline_axes: bool = False,
    include_envelope_row: bool = False,
) -> None:
    """Plot [Waveform | STFT Spectrogram | Overlaid Welch PSD] (+ optional RMS envelope) for 1..N WAVs."""
    n_tracks = len(tracks)
    n_rows = n_tracks + (1 if include_envelope_row else 0)
    fig_h = max(4.8, 3.1 * n_tracks + (2.6 if include_envelope_row else 0.6))
    fig = plt.figure(figsize=(16.5, fig_h), constrained_layout=True)
    gs = fig.add_gridspec(n_rows, 3, width_ratios=[1.25, 1.45, 1.05], hspace=0.22)
    fig.suptitle(title, fontsize=13.5, fontweight="bold")

    ax_psd = fig.add_subplot(gs[:, 2])
    ax_env = fig.add_subplot(gs[n_tracks, :2]) if include_envelope_row else None
    spec_axes: list[plt.Axes] = []
    last_mesh = None
    cmap = plt.get_cmap("inferno").copy()
    cmap.set_bad("black")

    loaded = []
    for label, wav_path, color in tracks:
        sr, x, meta = load_wav(wav_path, channel=channel)
        if meta["channels"] > 1:
            print(f"[INFO] {wav_path.name}: {meta['channels']} channels detected, analyzing channel {channel}.")
        loaded.append((label, sr, x, len(x) / sr, color))

    max_dur = max(d for _, _, _, d, _ in loaded)
    max_nyq = max(sr / 2.0 for _, sr, _, _, _ in loaded)
    max_abs_global = max(float(np.max(np.abs(x))) for _, _, x, _, _ in loaded)
    w_ylim = 0.85 if fixed_baseline_axes else max(0.05, min(1.05, 1.15 * max_abs_global))
    psd_min, psd_max = -105.0, -35.0
    env_min, env_max = -75.0, -5.0

    for row, (label, sr, x, dur, color) in enumerate(loaded):
        # Column 0: Time-domain waveform
        ax_w = fig.add_subplot(gs[row, 0])
        step = max(1, len(x) // 15000)
        nb = len(x) // step
        if nb > 0:
            xb = x[: nb * step].reshape(nb, step)
            tb = (np.arange(nb) + 0.5) * (step / sr)
            ax_w.fill_between(tb, np.min(xb, axis=1), np.max(xb, axis=1), color=color, alpha=0.85, linewidth=0.3)
        else:
            ax_w.plot(np.arange(len(x)) / sr, x, color=color, linewidth=0.8)

        if dur >= 2.0:
            ax_w.axvspan(0.0, 2.0, color="#94a3b8", alpha=0.22, label="0-2 s 前导参考" if row == 0 else None)
            ax_w.axvline(2.0, color="#475569", linestyle="--", linewidth=0.9)
            if row == 0:
                ax_w.legend(loc="upper right", fontsize=8, framealpha=0.9)
        ax_w.set_xlim(0, 30 if fixed_baseline_axes else max_dur)
        ax_w.set_ylim(-w_ylim, w_ylim)
        ax_w.set_ylabel("幅度 (FS)", fontsize=9)
        ax_w.set_title(f"{label} — 时域波形", fontsize=10, fontweight="bold", loc="left")
        ax_w.grid(True, linestyle=":", alpha=0.5)
        if row == n_tracks - 1 and not include_envelope_row:
            ax_w.set_xlabel("时间 (s)", fontsize=9)

        # Column 1: STFT Spectrogram (pad tiny clips and add sub-LSB floor to avoid log10(0))
        ax_s = fig.add_subplot(gs[row, 1])
        spec_axes.append(ax_s)
        nfft_s = min(512, max(64, 2 ** int(np.floor(np.log2(max(64, len(x) // 8))))))
        x_spec = x if len(x) >= nfft_s else np.pad(x, (0, nfft_s - len(x)))
        rng = np.random.default_rng(42 + row)
        _, _, _, im = ax_s.specgram(
            x_spec + rng.normal(0.0, 1e-7, len(x_spec)),
            NFFT=nfft_s,
            Fs=sr,
            noverlap=int(0.75 * nfft_s),
            cmap=cmap,
            vmin=-95,
            vmax=-25,
        )
        last_mesh = im
        if sr >= 400:
            ax_s.axhline(100.0, color="#38bdf8", linestyle="--", linewidth=0.9, alpha=0.9)
        if dur >= 2.0:
            ax_s.axvline(2.0, color="#e2e8f0", linestyle="--", linewidth=0.8, alpha=0.75)
        ax_s.set_xlim(0, 30 if fixed_baseline_axes else max_dur)
        ax_s.set_ylim(0, sr / 2.0)
        ax_s.set_ylabel("频率 (Hz)", fontsize=9)
        ax_s.set_title(f"{label} — STFT 语谱图 ({sr / 1000:g} kHz)", fontsize=10, fontweight="bold", loc="left")
        if row == n_tracks - 1 and not include_envelope_row:
            ax_s.set_xlabel("时间 (s)", fontsize=9)

        # Column 2: Welch PSD
        freqs_p, psd_db = welch_psd_db(x, sr, nfft=2048)
        f_min = min(30.0, 0.05 * max_nyq)
        mask = freqs_p >= f_min
        ax_psd.semilogx(freqs_p[mask], psd_db[mask], color=color, linewidth=1.7, label=label)
        if not fixed_baseline_axes and np.any(mask):
            psd_max = max(psd_max, float(np.max(psd_db[mask])) + 5.0)
            psd_min = min(psd_min, max(-130.0, float(np.percentile(psd_db[mask], 5)) - 10.0))

        # Optional bottom RMS envelope overlay
        if ax_env is not None:
            t_f, fdb, smooth = smoothed_frame_rms_db(x, sr, 20.0, 25)
            ax_env.plot(t_f, np.clip(fdb, -95, 0), color=color, alpha=0.25, linewidth=0.65)
            ax_env.plot(t_f, np.clip(smooth, -95, 0), color=color, linewidth=1.8, label=label)
            env_min = min(env_min, max(-95.0, float(np.percentile(smooth, 10)) - 15.0))
            env_max = max(env_max, min(2.0, float(np.max(smooth)) + 5.0))

    if last_mesh is not None and spec_axes:
        cbar = fig.colorbar(last_mesh, ax=spec_axes, fraction=0.025, pad=0.02)
        cbar.set_label("能量密度 (dBFS)", fontsize=9)

    if max_nyq >= 150.0:
        ax_psd.axvline(100.0, color="#0284c7", linestyle="--", linewidth=1.1, label="100 Hz HPF 截止线")
    if max_nyq >= 500.0:
        ax_psd.axvspan(300.0, min(4000.0, max_nyq), color="#cbd5e1", alpha=0.22, label="语音主能量带 (0.3-4 kHz)")
    ax_psd.set_xlim(min(30.0, 0.05 * max_nyq), max_nyq)
    ax_psd.set_ylim(psd_min, psd_max)
    ax_psd.set_xlabel("频率 (Hz, 对数坐标)", fontsize=9)
    ax_psd.set_ylabel("功率谱密度 (dBFS/Hz)", fontsize=9)
    ax_psd.set_title("Welch 平均功率谱对比 (LTASS / PSD)", fontsize=10, fontweight="bold")
    ax_psd.grid(True, which="both", linestyle=":", alpha=0.5)
    ax_psd.legend(loc="lower left", fontsize=8, framealpha=0.92)

    if ax_env is not None:
        if max_dur >= 2.0:
            ax_env.axvspan(0.0, 2.0, color="#94a3b8", alpha=0.20, label="0-2 s 前导参考")
            ax_env.axvline(2.0, color="#475569", linestyle="--", linewidth=0.9)
        ax_env.set_xlim(0, max_dur)
        ax_env.set_ylim(env_min, env_max)
        ax_env.set_xlabel("时间 (s)", fontsize=9.5)
        ax_env.set_ylabel("20 ms 帧能量 (dBFS)", fontsize=9.5)
        ax_env.set_title("短时能量包络对比 (20 ms 帧 RMS + 0.5 s 平滑)", fontsize=10, fontweight="bold", loc="left")
        ax_env.grid(True, linestyle=":", alpha=0.55)
        ax_env.legend(loc="lower right", ncol=min(4, len(tracks) + 1), fontsize=8.2, framealpha=0.92)

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


def print_metrics_report(files: list[tuple[str, Path]], channel: int = 0, show_delta: bool = False) -> None:
    """Print quantitative acoustic metrics (plus delta vs first track when show_delta=True)."""
    print("=== Acoustic Metrics Summary ===")
    metrics_list: list[tuple[str, dict[str, float]]] = []
    for label, p in files:
        m = compute_metrics(p, channel=channel)
        metrics_list.append((label, m))
        print(
            f"{label} | {int(m['ch'])}ch {int(m['bits'])}b {int(m['sr'])}Hz {m['dur']:.2f}s | "
            f"tot={m['tot_rms']:.2f}dBFS act={m['act_rms']:.2f}dBFS p10={m['noise_p10']:.2f}dBFS "
            f"pk={m['peak']:.2f}dBFS crest={m['crest']:.2f}dB | 0-2s={m['lead_rms']:.2f}dBFS | "
            f"<100Hz={m['sub100_pct']:.1f}% 0.3-4kHz={m['voice_pct']:.1f}% | "
            f"dc={m['dc']:+.1e} clips={int(m['clips'])}"
        )

    if show_delta and len(metrics_list) >= 2:
        ref_label, ref = metrics_list[0]
        print(f"=== Delta vs Reference [{ref_label}] ===")
        for label, m in metrics_list[1:]:
            print(
                f"{label} - {ref_label} | "
                f"dTot={m['tot_rms'] - ref['tot_rms']:+.2f}dB | "
                f"dNoiseP10={m['noise_p10'] - ref['noise_p10']:+.2f}dB | "
                f"dAct={m['act_rms'] - ref['act_rms']:+.2f}dB | "
                f"d0-2s={m['lead_rms'] - ref['lead_rms']:+.2f}dB | "
                f"d(<100Hz)={m['sub100_pct'] - ref['sub100_pct']:+.1f}%"
            )


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


def run_baseline() -> None:
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

    plot_tracks_diagnostic(
        "Set 1 稳态办公室场景 (Office) — 声学基线图谱 (16 kHz / 16-bit Mono)",
        set1_tracks,
        ASSETS_DIR / "set1_office_baseline.png",
        fixed_baseline_axes=True,
    )
    plot_tracks_diagnostic(
        "Set 2 动态地铁场景 (Metro) — 声学基线图谱 (16 kHz / 16-bit Mono)",
        set2_tracks,
        ASSETS_DIR / "set2_metro_baseline.png",
        fixed_baseline_axes=True,
    )
    plot_envelope_and_snr(ASSETS_DIR / "snr_envelope_comparison.png")

    all_wavs = sorted(SAMPLE_DIR.glob("*/*.wav"))
    print_metrics_report([(w.relative_to(SAMPLE_DIR).as_posix(), w) for w in all_wavs])


def run_custom(args: argparse.Namespace) -> None:
    """Analyze 1 or more arbitrary WAV files and export a unified diagnostic PNG."""
    wav_paths = resolve_wav_args(args.wavs)
    labels = args.labels if args.labels else [p.stem for p in wav_paths]
    if len(labels) != len(wav_paths):
        raise ValueError(f"--labels count ({len(labels)}) must match number of WAV files ({len(wav_paths)}).")

    tracks = [(lbl, p, PALETTE[i % len(PALETTE)]) for i, (lbl, p) in enumerate(zip(labels, wav_paths))]
    if args.out:
        out_path = Path(args.out).resolve()
    elif len(wav_paths) == 1:
        out_path = wav_paths[0].with_name(f"{wav_paths[0].stem}_acoustic_analysis.png")
    else:
        out_path = wav_paths[0].parent / f"{wav_paths[0].stem}_vs_{wav_paths[1].stem}_analysis.png"

    title = args.title or (
        f"音频声学分析图谱 — {labels[0]}"
        if len(tracks) == 1
        else f"多路音频声学对比分析 — {' vs '.join(labels)}"
    )
    plot_tracks_diagnostic(
        title,
        tracks,
        out_path,
        channel=args.channel,
        fixed_baseline_axes=False,
        include_envelope_row=True,
    )
    print(f"Saved acoustic plot: {out_path}")
    print_metrics_report(list(zip(labels, wav_paths)), channel=args.channel, show_delta=True)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Analyze arbitrary WAV file(s) or regenerate BleAudio acoustic baseline figures."
    )
    parser.add_argument("wavs", nargs="*", help="One or more PCM WAV files (supports wildcards) to analyze/compare.")
    parser.add_argument("--baseline", action="store_true", help="Regenerate the 3 audio baseline figures in sample/assets/.")
    parser.add_argument("-c", "--channel", type=int, default=0, help="Channel index for multi-channel WAVs (default: 0).")
    parser.add_argument("--labels", nargs="+", help="Custom legend labels matching input WAV files.")
    parser.add_argument("-o", "--out", type=str, help="Output PNG path for custom WAV analysis.")
    parser.add_argument("--title", type=str, help="Custom figure title.")
    args = parser.parse_args()

    try:
        if args.baseline or not args.wavs:
            run_baseline()
        else:
            run_custom(args)
    except (FileNotFoundError, ValueError, wave.Error) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
