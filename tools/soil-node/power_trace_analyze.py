#!/usr/bin/env python3
"""Analyze a calibrated soil-node current trace and firmware cycle log."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from datetime import datetime
from pathlib import Path
from statistics import median
from typing import Any


TIME_ALIASES = {
    "time",
    "times",
    "seconds",
    "sec",
    "timestamp",
    "relativetime",
    "elapsed",
    "elapsedtime",
}
CURRENT_ALIASES = {
    "current",
    "currenta",
    "currentamp",
    "currentamps",
    "currentma",
    "currentua",
    "i",
    "ia",
    "ima",
    "iua",
}
VOLTAGE_ALIASES = {
    "voltage",
    "voltagev",
    "volts",
    "v",
}


def normalized(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", value.lower())


def find_column(headers: list[str], aliases: set[str], kind: str) -> int:
    for index, header in enumerate(headers):
        if normalized(header) in aliases:
            return index
    raise ValueError(f"missing {kind} column; headers={headers!r}")


def is_time_header(header: str) -> bool:
    name = normalized(header)
    if name in TIME_ALIASES:
        return True
    return any(
        name == base + unit
        for base in ("time", "elapsed", "elapsedtime", "second")
        for unit in ("", "s", "ms", "us", "ns", "min")
    )


def parse_time(value: str, header: str) -> float:
    value = value.strip()
    try:
        result = float(value)
    except ValueError:
        try:
            result = datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
        except ValueError:
            parts = value.split(":")
            if len(parts) != 3:
                raise ValueError(f"unsupported time value: {value!r}") from None
            result = int(parts[0]) * 3600 + int(parts[1]) * 60 + float(parts[2])

    name = normalized(header)
    if "nanosecond" in name or name.endswith("ns"):
        result /= 1_000_000_000.0
    elif "microsecond" in name or name.endswith("us"):
        result /= 1_000_000.0
    elif "millisecond" in name or name.endswith("ms"):
        result /= 1_000.0
    elif "minute" in name or name.endswith("min"):
        result *= 60.0
    return result


def current_multiplier(header: str) -> float:
    name = normalized(header)
    if "microamp" in name or "ua" in name:
        return 1e-6
    if "milliamp" in name or "ma" in name:
        return 1e-3
    return 1.0


def finite_number(value: str, name: str) -> float:
    number = float(value.strip())
    if not math.isfinite(number):
        raise ValueError(f"non-finite {name}: {value!r}")
    return number


def load_trace(path: Path) -> tuple[list[float], list[float], list[float] | None]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        sample = handle.read(4096)
        handle.seek(0)
        try:
            dialect = csv.Sniffer().sniff(sample, delimiters=",;\t")
        except csv.Error:
            dialect = csv.excel
        reader = csv.reader(handle, dialect)
        try:
            headers = next(reader)
        except StopIteration:
            raise ValueError("trace CSV is empty") from None

        time_index = next(
            (index for index, header in enumerate(headers) if is_time_header(header)),
            None,
        )
        if time_index is None:
            raise ValueError(f"missing time column; headers={headers!r}")
        current_index = find_column(headers, CURRENT_ALIASES, "current")
        voltage_index = next(
            (i for i, header in enumerate(headers) if normalized(header) in VOLTAGE_ALIASES),
            None,
        )
        current_scale = current_multiplier(headers[current_index])
        times: list[float] = []
        currents: list[float] = []
        voltages: list[float] = []

        for line_number, row in enumerate(reader, start=2):
            if not row or all(not cell.strip() for cell in row):
                continue
            if max(time_index, current_index, voltage_index or 0) >= len(row):
                raise ValueError(f"short CSV row at line {line_number}")
            times.append(parse_time(row[time_index], headers[time_index]))
            currents.append(finite_number(row[current_index], "current") * current_scale)
            if voltage_index is not None:
                voltages.append(finite_number(row[voltage_index], "voltage"))

    if len(times) < 2:
        raise ValueError("at least two trace samples are required")
    if any(current < 0 for current in currents):
        raise ValueError("negative current values are not supported")
    if voltage_index is not None and any(voltage < 0 for voltage in voltages):
        raise ValueError("negative voltage values are not supported")
    return times, currents, voltages if voltage_index is not None else None


def clip_trace(
    times: list[float],
    currents: list[float],
    voltages: list[float] | None,
    start_s: float | None,
    end_s: float | None,
) -> tuple[list[float], list[float], list[float] | None]:
    origin = times[0]
    relative = [time - origin for time in times]
    lower = 0.0 if start_s is None else start_s
    upper = relative[-1] if end_s is None else end_s
    if upper <= lower:
        raise ValueError("end time must be greater than start time")
    selected = [i for i, value in enumerate(relative) if lower <= value <= upper]
    if len(selected) < 2:
        raise ValueError("selected range contains fewer than two samples")
    selected_voltages = None
    if voltages is not None:
        selected_voltages = [voltages[i] for i in selected]
    return (
        [relative[i] for i in selected],
        [currents[i] for i in selected],
        selected_voltages,
    )


def integrate_trace(
    times: list[float],
    currents: list[float],
    voltages: list[float] | None,
) -> dict[str, float]:
    charge_c = 0.0
    energy_j = 0.0
    for index in range(1, len(times)):
        delta = times[index] - times[index - 1]
        if delta <= 0:
            raise ValueError("time must increase strictly")
        charge_c += (currents[index] + currents[index - 1]) * 0.5 * delta
        if voltages is not None:
            power_previous = currents[index - 1] * voltages[index - 1]
            power_current = currents[index] * voltages[index]
            energy_j += (power_current + power_previous) * 0.5 * delta

    duration = times[-1] - times[0]
    return {
        "duration_s": duration,
        "charge_c": charge_c,
        "energy_j": energy_j,
        "mean_current_a": charge_c / duration,
        "peak_current_a": max(currents),
        "minimum_current_a": min(currents),
    }


def parse_device_log(path: Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8", errors="replace")
    active_ms = [float(value) for value in re.findall(r"CYCLE_ACTIVE_MS=(\d+)", text)]
    sleep_us = [float(value) for value in re.findall(r"DEEP_SLEEP_US=(\d+)", text)]
    return {
        "boots": text.count("*** Booting NuttX ***"),
        "successful_publishes": text.count("MQTT_PUBLISH=OK"),
        "verified_tls_sessions": text.count("cert=verified"),
        "soil_samples_ok": text.count("SOIL_OK"),
        "active_ms": active_ms,
        "median_active_ms": median(active_ms) if active_ms else None,
        "sleep_us": sleep_us,
        "median_sleep_s": median(sleep_us) / 1_000_000.0 if sleep_us else None,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="current-trace CSV")
    parser.add_argument("--device-log", type=Path)
    parser.add_argument("--start-s", type=float, help="relative capture start")
    parser.add_argument("--end-s", type=float, help="relative capture end")
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--cycle-seconds", type=float)
    parser.add_argument("--battery-mah", type=float, default=2000.0)
    parser.add_argument("--usable-fraction", type=float, default=0.8)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def analyze(args: argparse.Namespace) -> dict[str, Any]:
    if args.cycles < 1:
        raise ValueError("cycles must be at least one")
    if args.battery_mah <= 0 or not 0 < args.usable_fraction <= 1:
        raise ValueError("battery capacity and usable fraction must be positive")

    times, currents, voltages = load_trace(args.trace)
    times, currents, voltages = clip_trace(
        times, currents, voltages, args.start_s, args.end_s
    )
    result = integrate_trace(times, currents, voltages)
    cycle_seconds = args.cycle_seconds or result["duration_s"] / args.cycles
    if cycle_seconds <= 0:
        raise ValueError("cycle duration must be positive")
    charge_per_cycle = result["charge_c"] / args.cycles
    usable_charge_c = args.battery_mah * 3.6 * args.usable_fraction
    runtime_days = usable_charge_c / result["mean_current_a"] / 86400.0
    samples = len(times)

    output: dict[str, Any] = {
        "trace": str(args.trace),
        "samples": samples,
        "cycles": args.cycles,
        "cycle_seconds": round(cycle_seconds, 6),
        "duration_s": round(result["duration_s"], 6),
        "effective_sample_hz": round((samples - 1) / result["duration_s"], 3),
        "mean_current_ma": round(result["mean_current_a"] * 1000.0, 6),
        "peak_current_ma": round(result["peak_current_a"] * 1000.0, 6),
        "minimum_current_ua": round(result["minimum_current_a"] * 1_000_000.0, 3),
        "charge_per_cycle_mas": round(charge_per_cycle * 1000.0, 6),
        "battery_mah": args.battery_mah,
        "usable_fraction": args.usable_fraction,
        "projected_runtime_days": round(runtime_days, 3),
    }
    if voltages is not None:
        output["energy_per_cycle_j"] = round(result["energy_j"] / args.cycles, 6)
        output["mean_voltage_v"] = round(sum(voltages) / len(voltages), 6)
    if args.device_log:
        output["device_log"] = parse_device_log(args.device_log)
    return output


def main() -> int:
    args = parse_args()
    try:
        result = analyze(args)
    except (OSError, ValueError, csv.Error) as exc:
        raise SystemExit(f"power trace analysis failed: {exc}") from exc
    payload = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
