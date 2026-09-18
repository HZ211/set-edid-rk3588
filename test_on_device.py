#!/usr/bin/env python3
"""Exercise -r on the RK3588 HDMI-RX board and restore its original EDID."""

import argparse
import csv
import hashlib
import json
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


DEFAULT_MODES = [
    "1920x1080@60",
    "640x480@60",
    "2560x1080@60",
    "2560x1080@75",
    "2560x1440@30",
    "2560x1440@60",
    "2560x1440@75",
    "2560x1440@120",
    "2560x1440@144",
    "2560x1440@165",
    "3440x1440@60",
    "3440x1440@75",
    "3440x1440@100",
    "3440x1440@120",
    "3440x1440@144",
    "3840x2160@24",
    "3840x2160@25",
    "3840x2160@30",
    "3840x2160@50",
    "3840x2160@60",
    "4096x2160@24",
    "4096x2160@25",
    "4096x2160@30",
    "4096x2160@50",
    "4096x2160@60",
]


def run(*args):
    return subprocess.run(args, capture_output=True, check=False)


def decode(data):
    return data.decode("utf-8", errors="replace")


def get_edid(device):
    result = run("v4l2-ctl", "-d", device, "--get-edid=pad=0,format=raw")
    if result.returncode:
        raise RuntimeError("EDID read failed: " + decode(result.stderr))
    if len(result.stdout) not in (128, 256, 384, 512):
        raise RuntimeError(f"Unexpected EDID length: {len(result.stdout)}")
    return result.stdout


def parse_generated(text):
    array = re.search(r"generated_edid_data\[\d+\]\s*=\s*\{(.*?)\};", text, re.S)
    if not array:
        raise ValueError("No generated EDID array")
    return bytes(int(number, 16) for number in re.findall(r"0x([0-9a-fA-F]{2})", array.group(1)))


def parse_timing(text):
    width = re.search(r"Active width:\s*(\d+)", text)
    height = re.search(r"Active height:\s*(\d+)", text)
    fps = re.search(r"\(([\d.]+) frames per second\)", text)
    if not (width and height and fps):
        return None
    return (int(width.group(1)), int(height.group(1)), float(fps.group(1)))


def query_timing(device):
    result = run("v4l2-ctl", "-d", device, "--query-dv-timings")
    output = decode(result.stdout + result.stderr)
    return parse_timing(output) if result.returncode == 0 else None, output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("modes", nargs="*", default=DEFAULT_MODES)
    parser.add_argument("--device", default="/dev/video0")
    parser.add_argument("--settle", type=float, default=12.0)
    parser.add_argument("--confirm", type=float, default=3.0)
    args = parser.parse_args()
    if args.settle < 0 or args.confirm < 0:
        parser.error("wait times must be nonnegative")

    root = Path(__file__).resolve().parent
    binary = root / "set_edid"
    if not binary.is_file():
        parser.error(f"binary not found: {binary}")

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    result_dir = root / f"test-results-{stamp}"
    result_dir.mkdir(exist_ok=False)
    original = get_edid(args.device)
    original_path = result_dir / "original-edid.bin"
    original_path.write_bytes(original)
    print(f"Original EDID: {len(original)} bytes, sha256={hashlib.sha256(original).hexdigest()}", flush=True)

    rows = []
    restore_error = None
    try:
        for mode in args.modes:
            requested = re.fullmatch(r"(\d+)x(\d+)@(\d+)", mode)
            if not requested:
                print(f"Invalid test mode: {mode}", file=sys.stderr)
                continue
            expected = tuple(map(int, requested.groups()))
            log = {"mode": mode, "expected": expected}
            generated = run(str(binary), "-r", mode, "-n")
            generated_text = decode(generated.stdout + generated.stderr)
            log["generate_output"] = generated_text
            row = {"mode": mode, "result": "", "actual": "", "edid_match": "", "detail": ""}
            if generated.returncode:
                row["result"] = "GENERATE_FAIL"
                row["detail"] = generated_text.strip().splitlines()[-1] if generated_text.strip() else "no output"
            else:
                try:
                    expected_edid = parse_generated(generated_text)
                    log["generated_edid_sha256"] = hashlib.sha256(expected_edid).hexdigest()
                    applied = run(str(binary), "-r", mode)
                    log["apply_output"] = decode(applied.stdout + applied.stderr)
                    if applied.returncode:
                        row["result"] = "SET_FAIL"
                        row["detail"] = log["apply_output"].strip().splitlines()[-1]
                    else:
                        readback = get_edid(args.device)
                        row["edid_match"] = "yes" if readback == expected_edid else "no"
                        log["readback_edid_sha256"] = hashlib.sha256(readback).hexdigest()
                        time.sleep(args.settle)
                        first, first_text = query_timing(args.device)
                        log["first_timing"] = first_text
                        time.sleep(args.confirm)
                        second, second_text = query_timing(args.device)
                        log["second_timing"] = second_text
                        actual = second or first
                        row["actual"] = "none" if actual is None else f"{actual[0]}x{actual[1]}@{actual[2]:.2f}"
                        if readback != expected_edid:
                            row["result"] = "EDID_MISMATCH"
                        elif first is None and second is None:
                            row["result"] = "NO_LOCK"
                        elif (first is None or second is None or
                              first[:2] != second[:2] or
                              abs(first[2] - second[2]) >= 0.2):
                            row["result"] = "UNSTABLE"
                        elif actual[:2] == expected[:2] and abs(actual[2] - expected[2]) < 0.5:
                            row["result"] = "PASS"
                        else:
                            row["result"] = "FALLBACK"
                except Exception as exc:
                    row["result"] = "TEST_ERROR"
                    row["detail"] = str(exc)
            rows.append(row)
            (result_dir / f"{mode}.json").write_text(json.dumps(log, ensure_ascii=False, indent=2) + "\n")
            print(f"{mode}: {row['result']} actual={row['actual']} edid_match={row['edid_match']}", flush=True)
    finally:
        restored = run("v4l2-ctl", "-d", args.device,
                       f"--set-edid=pad=0,file={original_path},format=raw")
        if restored.returncode:
            restore_error = decode(restored.stderr)
        else:
            try:
                if get_edid(args.device) != original:
                    restore_error = "readback differs from original EDID"
            except Exception as exc:
                restore_error = str(exc)
        print("Restore original EDID: " + ("OK" if restore_error is None else "FAILED: " + restore_error), flush=True)
        if rows:
            with (result_dir / "summary.csv").open("w", newline="") as output:
                writer = csv.DictWriter(output, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            print(f"Results: {result_dir / 'summary.csv'}", flush=True)
    return 1 if restore_error is not None else 0


if __name__ == "__main__":
    sys.exit(main())
