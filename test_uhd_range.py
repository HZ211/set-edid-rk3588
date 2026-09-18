#!/usr/bin/env python3
"""Check generated UHD/DCI range limits without writing to /dev/video0."""

import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent
MODES = (
    (width, 2160, refresh)
    for width in (3840, 4096)
    for refresh in (24, 25, 30, 50, 60)
)


def generated_edid(mode):
    completed = subprocess.run(
        [str(ROOT / "set_edid"), "-r", mode, "-n"],
        capture_output=True, text=True, check=True,
    )
    array = re.search(r"generated_edid_data\[\d+\]\s*=\s*\{(.*?)\};",
                      completed.stdout, re.S)
    assert array is not None, f"No generated array for {mode}"
    return bytes(int(value, 16)
                 for value in re.findall(r"0x([0-9a-fA-F]{2})", array.group(1)))


def main():
    with tempfile.TemporaryDirectory(prefix="set-edid-range-") as temporary:
        raw_path = Path(temporary) / "edid.bin"
        for width, height, refresh in MODES:
            mode = f"{width}x{height}@{refresh}"
            edid = generated_edid(mode)
            assert len(edid) == 256 and edid[126] == 1, mode
            assert all(sum(edid[start:start + 128]) % 256 == 0
                       for start in (0, 128)), mode
            assert edid[108:113] == bytes((0, 0, 0, 0xfd, 0)), mode
            vertical_min, vertical_max = edid[113:115]
            horizontal_min, horizontal_max = edid[115:117]
            clock_limit_khz = edid[117] * 10000
            assert (vertical_min, vertical_max) == (refresh - 5, refresh + 5), mode

            # The CTA mode table uses 297 or 594 MHz and a 2250-line frame.
            pixel_clock_khz = 297000 if refresh in (24, 25, 30) else 594000
            htotal = pixel_clock_khz * 1000 // (2250 * refresh)
            horizontal_khz = pixel_clock_khz / htotal
            assert horizontal_min <= horizontal_khz <= horizontal_max, mode
            assert pixel_clock_khz <= clock_limit_khz, mode

            raw_path.write_bytes(edid)
            decoded = subprocess.run(["edid-decode", str(raw_path)],
                                     capture_output=True, text=True)
            assert decoded.returncode == 0, (mode, decoded.stdout, decoded.stderr)
            assert "FAIL" not in decoded.stdout + decoded.stderr, mode
            print(f"PASS {mode}: V {vertical_min}-{vertical_max} Hz, "
                  f"H {horizontal_min}-{horizontal_max} kHz, "
                  f"clock <= {clock_limit_khz // 1000} MHz")


if __name__ == "__main__":
    main()
