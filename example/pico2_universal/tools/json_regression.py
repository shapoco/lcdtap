#!/usr/bin/env python3
"""Byte-level regression harness for the JSON serial interface.

Captures the raw response bytes of a fixed command sequence so that a
refactoring of the JSON engine can be proven wire-identical.

Usage:
  # Capture a baseline with the reference firmware:
  python json_regression.py --port /dev/ttyACM0 --save baseline/

  # After flashing the refactored firmware, compare:
  python json_regression.py --port /dev/ttyACM0 --compare baseline/

getstats values change between runs, so that response is compared
structurally (names/units/fmt only). Every other response must match byte
for byte. The sequence avoids reboot-triggering setparams (it echoes the
current values back), so it can be run repeatedly without reconnecting.

Requires pyserial: pip install pyserial
"""

import argparse
import json
import pathlib
import sys
import time

import serial


def send_command(port, cmd_obj, timeout_s=60.0):
    """Send one command object and return the raw response line (bytes,
    including the trailing CRLF)."""
    port.reset_input_buffer()
    port.write((json.dumps(cmd_obj) + "\r\n").encode("ascii"))
    port.flush()

    deadline = time.monotonic() + timeout_s
    buf = bytearray()
    while time.monotonic() < deadline:
        chunk = port.read(4096)
        if chunk:
            buf.extend(chunk)
            if buf.endswith(b"\r\n"):
                return bytes(buf)
        else:
            time.sleep(0.005)
    raise TimeoutError(f"no complete response for {cmd_obj}")


def stats_structure(raw):
    """Reduce a getstats response to its structure (drop the values)."""
    obj = json.loads(raw.decode("ascii"))
    return [
        {k: e[k] for k in ("name", "unit", "fmt")} for e in obj.get("stats", [])
    ]


def build_sequence(port):
    """Yield (name, raw_response, structural_only) tuples."""
    yield "hello", send_command(port, {"command": "hello"}, 5), False

    presets_raw = send_command(port, {"command": "getpresets"}, 5)
    yield "getpresets", presets_raw, False

    params_raw = send_command(port, {"command": "getparams"}, 10)
    yield "getparams", params_raw, False

    presets = json.loads(presets_raw.decode("ascii"))["presets"]
    yield (
        "getparams_preset",
        send_command(port, {"command": "getparams", "preset": presets[0]}, 10),
        False,
    )

    yield "getstats", send_command(port, {"command": "getstats"}, 5), True
    yield "statsreset", send_command(port, {"command": "statsreset"}, 5), False

    # setparams echoing the live values back: exercises the full parse +
    # commit path without changing anything, so no reboot is triggered.
    params = json.loads(params_raw.decode("ascii"))["params"]
    echo = {p["id"]: p["value"] for p in params}
    yield (
        "setparams_echo",
        send_command(port, {"command": "setparams", "params": echo}, 10),
        False,
    )

    yield (
        "getparams_after_echo",
        send_command(port, {"command": "getparams"}, 10),
        False,
    )

    # writeProtected engages and releases along the success path. With no
    # input attached the framebuffer holds the static splash, so the payload
    # is deterministic.
    yield (
        "getframebuffer",
        send_command(
            port, {"command": "getframebuffer", "writeProtected": True}, 120
        ),
        False,
    )

    yield (
        "cmddump_start",
        send_command(port, {"command": "cmddump_start"}, 5),
        False,
    )
    yield (
        "cmddump_status1",
        send_command(port, {"command": "cmddump_getstatus"}, 5),
        False,
    )
    yield (
        "cmddump_forcetrigger",
        send_command(port, {"command": "cmddump_forcetrigger"}, 5),
        False,
    )
    yield (
        "cmddump_abort",
        send_command(port, {"command": "cmddump_abort"}, 5),
        False,
    )
    yield (
        "cmddump_status2",
        send_command(port, {"command": "cmddump_getstatus"}, 5),
        False,
    )
    yield (
        "cmddump_read",
        send_command(port, {"command": "cmddump_read"}, 30),
        False,
    )

    # Error paths must stay stable too.
    yield (
        "unknown_command",
        send_command(port, {"command": "nosuchcommand"}, 5),
        False,
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--save", metavar="DIR", help="capture a baseline")
    mode.add_argument("--compare", metavar="DIR", help="diff against baseline")
    args = ap.parse_args()

    out_dir = pathlib.Path(args.save or args.compare)
    port = serial.Serial(args.port, 115200, timeout=0.05)
    time.sleep(0.3)  # let the CDC connection settle
    port.reset_input_buffer()

    failures = 0
    for idx, (name, raw, structural) in enumerate(build_sequence(port)):
        fname = out_dir / f"{idx:02d}_{name}.bin"
        if args.save:
            out_dir.mkdir(parents=True, exist_ok=True)
            fname.write_bytes(raw)
            print(f"saved   {fname} ({len(raw)} bytes)")
            continue

        expected = fname.read_bytes()
        if structural:
            ok = stats_structure(expected) == stats_structure(raw)
        else:
            ok = expected == raw
        if ok:
            print(f"OK      {name} ({len(raw)} bytes)")
        else:
            failures += 1
            (out_dir / f"{idx:02d}_{name}.actual").write_bytes(raw)
            print(f"DIFFER  {name}: see {fname} vs .actual")

    if args.compare:
        if failures:
            print(f"\n{failures} response(s) differ")
            sys.exit(1)
        print("\nall responses match")


if __name__ == "__main__":
    main()
