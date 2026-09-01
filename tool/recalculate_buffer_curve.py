#!/usr/bin/env python3
"""Recalculate buffer-energy curves from recorded 10 Hz STA power samples."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt

from plot_power_curve import iter_samples


BUFFER_MAX_J = 60.0
SETTLEMENT_PERIOD_S = 0.1


def recalculate(
    power_w: list[float], limit_w: float, initial_buffer_j: float = BUFFER_MAX_J,
    buffer_max_j: float = BUFFER_MAX_J,
) -> list[float]:
    """Apply the firmware's 100 ms buffer accounting to recorded PAVG data."""
    energy = min(buffer_max_j, max(0.0, initial_buffer_j))
    result = []
    for measured_power in power_w:
        energy += (limit_w - measured_power) * SETTLEMENT_PERIOD_S
        energy = min(buffer_max_j, max(0.0, energy))
        result.append(energy)
    return result


def plot_single_limit(
    times: list[float], powers: list[float], recorded_buffer: list[float],
    limit_w: float, initial_buffer_j: float, buffer_max_j: float, output: Path
) -> None:
    energy = recalculate(powers, limit_w, initial_buffer_j, buffer_max_j)
    fig, axes = plt.subplots(2, 1, sharex=True, figsize=(14, 8), constrained_layout=True)
    axes[0].plot(times, powers, color="#1565c0", linewidth=0.9, label="Recorded PAVG")
    axes[0].axhline(limit_w, color="#d32f2f", linestyle="--", linewidth=1.1, label=f"Limit {limit_w:g} W")
    axes[0].set_ylabel("Power (W)")
    axes[0].set_title(f"Power limit {limit_w:g} W, buffer {buffer_max_j:g} J")
    axes[0].grid(alpha=0.25)
    axes[0].legend(loc="upper right")

    axes[1].plot(times, energy, color="#2e7d32", linewidth=1.0, label="Recalculated buffer")
    axes[1].plot(times, recorded_buffer, color="#777777", linestyle=":", linewidth=0.9, label="Recorded buffer")
    axes[1].set_ylabel("Buffer energy (J)")
    axes[1].set_xlabel("Elapsed time (s)")
    axes[1].set_ylim(-5.0, buffer_max_j + 5.0)
    axes[1].grid(alpha=0.25)
    axes[1].legend(loc="lower left")
    fig.suptitle(f"Recalculated buffer curve; limit={limit_w:g} W; max={buffer_max_j:g} J")
    fig.savefig(output, dpi=150)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description="Recalculate buffer curves for several power limits")
    parser.add_argument("input", type=Path, help="STA serial log text file")
    parser.add_argument("-o", "--output", type=Path, help="PNG output path")
    parser.add_argument("--start-time", type=float, default=150.0)
    parser.add_argument("--window-seconds", type=float, default=50.0)
    parser.add_argument("--limits", type=float, nargs="+", default=[30.0, 25.0, 20.0, 15.0])
    parser.add_argument("--initial-buffer", type=float, default=BUFFER_MAX_J, help="initial buffer energy in J")
    parser.add_argument("--buffer-max", type=float, default=BUFFER_MAX_J, help="buffer energy upper limit in J")
    parser.add_argument("--separate", action="store_true", help="write one plot per power limit")
    args = parser.parse_args()

    selected = [
        sample for sample in iter_samples(args.input)
        if args.start_time <= sample.index / 10.0 < args.start_time + args.window_seconds
    ]
    if not selected:
        raise SystemExit("no STA frames in requested time range")

    times = [sample.index / 10.0 for sample in selected]
    powers = [sample.power_w for sample in selected]
    output = args.output or args.input.with_name(
        f"{args.input.stem}_buffer_{args.start_time:g}_{args.start_time + args.window_seconds:g}.png"
    )

    if args.buffer_max <= 0.0 or args.buffer_max > BUFFER_MAX_J:
        parser.error(f"--buffer-max must be greater than 0 and at most {BUFFER_MAX_J:g} J")
    if args.initial_buffer < 0.0 or args.initial_buffer > args.buffer_max:
        parser.error(f"--initial-buffer must be between 0 and {args.buffer_max:g} J")

    if args.separate:
        for limit in args.limits:
            separate_output = output.with_name(f"{output.stem}_{limit:g}W{output.suffix}")
            plot_single_limit(times, powers, [sample.buffer_j for sample in selected], limit, args.initial_buffer, args.buffer_max, separate_output)
            final_energy = recalculate(powers, limit, args.initial_buffer, args.buffer_max)[-1]
            print(f"limit {limit:g} W: final buffer {final_energy:.1f} J")
            print(f"saved: {separate_output}")
        return

    fig, axes = plt.subplots(2, 1, sharex=True, figsize=(14, 8), constrained_layout=True)
    axes[0].plot(times, powers, color="#1565c0", linewidth=0.9, label="Recorded PAVG")
    axes[0].axhline(35.0, color="#ef6c00", linestyle="--", linewidth=1.0, label="Original 35 W")
    for limit in args.limits:
        axes[0].axhline(limit, linestyle=":", linewidth=0.9, label=f"Limit {limit:g} W")
    axes[0].set_ylabel("Power (W)")
    axes[0].set_title(f"Recorded power, {args.start_time:g}-{args.start_time + args.window_seconds:g} s")
    axes[0].grid(alpha=0.25)
    axes[0].legend(loc="upper right", ncol=3)

    for limit in args.limits:
        energy = recalculate(powers, limit, args.initial_buffer, args.buffer_max)
        axes[1].plot(times, energy, linewidth=1.0, label=f"Limit {limit:g} W")
    original = [sample.buffer_j for sample in selected]
    axes[1].plot(times, original, color="#555555", linewidth=0.9, linestyle="--", label="Recorded 35 W buffer")
    axes[1].set_ylabel("Buffer energy (J)")
    axes[1].set_xlabel("Elapsed time (s)")
    # Keep visual margin while retaining the physical 0..60 J clamp in
    # recalculate(). Any future out-of-range data is clipped to this view.
    axes[1].set_ylim(-5.0, args.buffer_max + 5.0)
    axes[1].grid(alpha=0.25)
    axes[1].legend(loc="lower left", ncol=3)
    fig.suptitle(f"Recalculated buffer curves; {len(selected)} samples")
    fig.savefig(output, dpi=150)
    plt.close(fig)

    print(f"samples: {len(selected)}")
    print(f"saved: {output}")
    for limit in args.limits:
        final_energy = recalculate(powers, limit, args.initial_buffer, args.buffer_max)[-1]
        print(f"limit {limit:g} W: final buffer {final_energy:.1f} J")


if __name__ == "__main__":
    main()
