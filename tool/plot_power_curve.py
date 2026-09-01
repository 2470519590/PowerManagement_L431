#!/usr/bin/env python3
"""Stream-parse电管STA日志并绘制功率、电流、缓冲能量曲线。"""

from __future__ import annotations

import argparse
import csv
import math
import re
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt


STA_RE = re.compile(r"^STA,(?P<body>.*)$")


@dataclass
class Sample:
    index: int
    power_w: float
    current_ma: float
    buffer_j: float
    limit_w: float
    cut: bool
    ocp: bool
    overload: bool


def parse_sta_line(line: str, index: int) -> Sample | None:
    """Parse one firmware STA line; unrelated log lines return None."""
    match = STA_RE.match(line.strip())
    if match is None:
        return None

    fields: dict[str, str] = {}
    for item in match.group("body").split(","):
        key, separator, value = item.partition("=")
        if not separator:
            return None
        fields[key] = value

    required = ("PAVG", "IAVG", "BUF", "LIM", "CUT", "OCP", "OV")
    if any(key not in fields for key in required):
        return None
    try:
        return Sample(
            index=index,
            power_w=float(fields["PAVG"]),
            current_ma=float(fields["IAVG"]),
            buffer_j=float(fields["BUF"]),
            limit_w=float(fields["LIM"]),
            cut=fields["CUT"] == "1",
            ocp=fields["OCP"] == "1",
            overload=fields["OV"] == "1",
        )
    except ValueError:
        return None


def iter_samples(path: Path):
    """Yield valid samples without loading the input file into memory."""
    valid_index = 0
    with path.open("r", encoding="utf-8-sig", errors="replace") as stream:
        for line in stream:
            sample = parse_sta_line(line, valid_index)
            if sample is not None:
                yield sample
                valid_index += 1


def compress(samples, max_points: int) -> list[Sample]:
    """Keep bucket min/max points to preserve long-log power peaks."""
    if max_points < 4:
        raise ValueError("max_points must be at least 4")

    if len(samples) <= max_points:
        return samples[:]
    result: list[Sample] = []
    bucket_count = max(1, max_points // 2)
    bucket_size = math.ceil(len(samples) / bucket_count)
    for start in range(0, len(samples), bucket_size):
        bucket = samples[start : start + bucket_size]
        result.append(min(bucket, key=lambda item: item.power_w))
        result.append(max(bucket, key=lambda item: item.power_w))
    return sorted({item.index: item for item in result}.values(), key=lambda item: item.index)


def _compress_bucket(bucket: list[Sample]) -> list[Sample]:
    if len(bucket) <= 4:
        return bucket[:]
    selected = [bucket[0]]
    selected.append(min(bucket[1:-1], key=lambda item: item.power_w))
    selected.append(max(bucket[1:-1], key=lambda item: item.power_w))
    selected.append(bucket[-1])
    return sorted({item.index: item for item in selected}.values(), key=lambda item: item.index)


def collect_samples(path: Path, max_points: int) -> tuple[list[Sample], int]:
    """Stream the log and retain a bounded min/max representation."""
    # Keep a small bucket only; memory use is independent of log length.
    samples: list[Sample] = []
    bucket: list[Sample] = []
    total = 0
    bucket_size = 1
    # Adapt the bucket size as the stream grows. Existing points are never
    # duplicated and the retained list remains bounded after each merge.
    for sample in iter_samples(path):
        total += 1
        bucket.append(sample)
        if len(bucket) >= bucket_size:
            samples.extend(_compress_bucket(bucket))
            bucket.clear()
            if len(samples) > max_points * 2:
                bucket_size *= 2
                samples = compress(samples, max_points)
    if bucket:
        samples.extend(_compress_bucket(bucket))
    if len(samples) > max_points:
        samples = compress(samples, max_points)
    return samples, total


def write_csv(path: Path, samples: list[Sample]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("sample", "time_s", "power_w", "current_ma", "buffer_j", "limit_w", "cut", "ocp", "overload"))
        for item in samples:
            writer.writerow((item.index, f"{item.index / 10:.1f}", item.power_w, item.current_ma, item.buffer_j, item.limit_w, int(item.cut), int(item.ocp), int(item.overload)))


def plot(samples: list[Sample], total: int, output: Path, title: str) -> None:
    if not samples:
        raise ValueError("no valid STA lines found")

    time_s = [item.index / 10.0 for item in samples]
    power = [item.power_w for item in samples]
    current = [item.current_ma / 1000.0 for item in samples]
    buffer = [item.buffer_j for item in samples]
    limit = [item.limit_w for item in samples]

    fig, axes = plt.subplots(3, 1, sharex=True, figsize=(14, 9), constrained_layout=True)
    axes[0].plot(time_s, power, color="#1565c0", linewidth=0.8, label="PAVG")
    axes[0].plot(time_s, limit, color="#d32f2f", linewidth=1.0, label="LIM")
    axes[0].axhline(28.0, color="#ef6c00", linestyle="--", linewidth=1.2, label="28 W")
    axes[0].set_ylabel("Power (W)")
    axes[0].set_title(title)
    axes[0].legend(loc="upper right")
    axes[0].grid(alpha=0.25)

    axes[1].plot(time_s, current, color="#2e7d32", linewidth=0.8)
    axes[1].axhline(4.0, color="#d32f2f", linestyle="--", linewidth=1.0, label="4 A")
    axes[1].set_ylabel("Current (A)")
    axes[1].legend(loc="upper right")
    axes[1].grid(alpha=0.25)

    axes[2].plot(time_s, buffer, color="#6a1b9a", linewidth=0.8)
    axes[2].set_ylabel("Buffer (J)")
    axes[2].set_xlabel("Elapsed time (s)")
    axes[2].set_ylim(bottom=0)
    axes[2].grid(alpha=0.25)

    cut_points = [time_s[i] for i, item in enumerate(samples) if item.cut]
    ocp_points = [time_s[i] for i, item in enumerate(samples) if item.ocp]
    for axis in axes:
        if cut_points:
            for point in cut_points:
                axis.axvline(point, color="#e65100", alpha=0.18, linewidth=0.7)
        if ocp_points:
            for point in ocp_points:
                axis.axvline(point, color="#b71c1c", alpha=0.22, linewidth=0.7)

    fig.suptitle(f"Valid STA frames: {total}; plotted points: {len(samples)}")
    fig.savefig(output, dpi=150)
    plt.close(fig)


def plot_segments(
    samples: list[Sample], total: int, output: Path, start_s: float, window_s: float, count: int
) -> list[Path]:
    """Write consecutive fixed-duration plots using the original time axis."""
    outputs: list[Path] = []
    for segment_index in range(count):
        segment_start = start_s + segment_index * window_s
        segment_end = segment_start + window_s
        segment = [
            item for item in samples
            if segment_start <= (item.index / 10.0) < segment_end
        ]
        if not segment:
            continue
        segment_output = output.with_name(
            f"{output.stem}_{segment_start:g}_{segment_end:g}{output.suffix}"
        )
        plot(segment, total, segment_output, f"Power curve {segment_start:g}-{segment_end:g} s")
        outputs.append(segment_output)
    return outputs


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot power-management STA log")
    parser.add_argument("input", type=Path, help="serial log text file")
    parser.add_argument("-o", "--output", type=Path, help="PNG output path")
    parser.add_argument("--csv", type=Path, help="optional compressed CSV output")
    parser.add_argument("--max-points", type=int, default=20000, help="maximum retained plot points")
    parser.add_argument("--start-time", type=float, help="start time for segmented plots, seconds")
    parser.add_argument("--window-seconds", type=float, default=50.0, help="duration of each segmented plot")
    parser.add_argument("--count", type=int, default=5, help="number of segmented plots")
    args = parser.parse_args()

    output = args.output or args.input.with_suffix(".png")
    samples, total = collect_samples(args.input, args.max_points)
    if args.start_time is None:
        plot(samples, total, output, args.input.name)
        plotted_outputs = [output]
    else:
        if args.window_seconds <= 0 or args.count <= 0:
            parser.error("--window-seconds and --count must be positive")
        plotted_outputs = plot_segments(
            samples, total, output, args.start_time, args.window_seconds, args.count
        )
        if not plotted_outputs:
            raise SystemExit("no STA frames in requested time range")
    if args.csv:
        write_csv(args.csv, samples)
    print(f"valid STA frames: {total}")
    print(f"plot points: {len(samples)}")
    for plotted_output in plotted_outputs:
        print(f"saved: {plotted_output}")
    if args.csv:
        print(f"saved: {args.csv}")


if __name__ == "__main__":
    main()
