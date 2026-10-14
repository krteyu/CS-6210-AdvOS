#!/usr/bin/env python3
"""End-to-end TinyFile tests using the bundled Snappy C decoder (no pip packages).

Usage (from Project2/ipcs):
    python3 tests/tinyfile_test.py .
    python3 tests/tinyfile_test.py . --matrix

Never run while another TinyFile service is already running.
"""
import argparse
import ctypes
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time


def build_decoder(ipcs: Path, temp: Path):
    so = temp / "libtinyfile_snappy_check.so"
    subprocess.run([
        "gcc", "-std=gnu11", "-O2", "-fPIC", "-shared",
        "-I", str(ipcs / "snappy"),
        str(ipcs / "snappy/snappy.c"), "-o", str(so),
    ], check=True)
    lib = ctypes.CDLL(str(so))
    lib.snappy_uncompressed_length.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    lib.snappy_uncompressed_length.restype = ctypes.c_bool
    lib.snappy_uncompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    lib.snappy_uncompress.restype = ctypes.c_int
    return lib


def verify_snappy(lib, original: Path, produced: Path):
    if not produced.is_file():
        return f"missing output: {produced}"
    src = original.read_bytes()
    encoded = produced.read_bytes()
    if not encoded:
        return "empty output (invalid raw Snappy block)"
    srcbuf = ctypes.create_string_buffer(encoded, len(encoded))
    size = ctypes.c_size_t()
    if not lib.snappy_uncompressed_length(srcbuf, len(encoded), ctypes.byref(size)):
        return "invalid Snappy header"
    if size.value != len(src):
        return f"Snappy claims {size.value} decoded bytes; input has {len(src)} bytes"
    decoded = ctypes.create_string_buffer(max(size.value, 1))
    error = lib.snappy_uncompress(srcbuf, len(encoded), decoded)
    if error != 0:
        return f"Snappy decoder returned {error}"
    if decoded.raw[:size.value] != src:
        return "decompressed bytes differ from input"
    return None


def wait_ready(p, logpath):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if p.poll() is not None:
            raise RuntimeError(f"server exited early: {logpath.read_text(errors='replace')}")
        if logpath.exists() and "TINYFILE_READY\n" in logpath.read_text(errors="replace"):
            return
        time.sleep(0.05)
    raise RuntimeError(f"server never printed TINYFILE_READY; log: {logpath}")


def run_one(ipcs, lib, temp, files, mode, label, timeout=35):
    output = temp / "outputs" / label
    output.mkdir(parents=True, exist_ok=True)
    if len(files) == 1:
        input_args = ["--file", str(files[0])]
    else:
        filelist = temp / f"{label}.txt"
        filelist.write_text("\n".join(str(f) for f in files) + "\n")
        input_args = ["--files", str(filelist)]
    command = ["./run-client.sh", *input_args, "--state", mode, "--output_dir", str(output)]
    try:
        r = subprocess.run(command, cwd=ipcs, text=True, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return f"client timed out after {timeout}s"
    if r.returncode:
        return f"client exited {r.returncode}: {(r.stderr + r.stdout)[-500:]}"
    timing_lines = [line for line in r.stdout.splitlines() if line.startswith("TINYFILE_CST|")]
    if len(timing_lines) != len(files):
        return f"expected {len(files)} CST lines, received {len(timing_lines)}: {r.stdout[-500:]}"
    for f in files:
        expected_log = f"TINYFILE_CST|file={f.name}|mode={mode}|seconds="
        if not any(line.startswith(expected_log) for line in timing_lines):
            return f"missing valid CST line for {f.name}"
        err = verify_snappy(lib, f, output / (f.name + ".snappy"))
        if err:
            return f"{f.name}: {err}"
    return None


def test_server(ipcs, lib, temp, n_sms, sms_size, mode, files, label):
    infofile = Path("/tmp/tinyfile_server_info.txt")
    if infofile.exists():
        raise RuntimeError("TinyFile info file already exists: stop the old server before running tests")
    logpath = temp / f"server_{label}.log"
    with logpath.open("w+") as log:
        p = subprocess.Popen(["./run-server.sh", "--n_sms", str(n_sms), "--sms_size", str(sms_size)],
                             cwd=ipcs, stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_ready(p, logpath)
            error = run_one(ipcs, lib, temp, files, mode, label)
        finally:
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try:
                    p.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
    if infofile.exists():
        return "server did not remove its info file on exit"
    if p.returncode != 0:
        return f"server returned exit status {p.returncode}; see {logpath}"
    return error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ipcs", nargs="?", default=".", help="path to Project2/ipcs")
    parser.add_argument("--matrix", action="store_true", help="run all README segment-count/size combinations")
    args = parser.parse_args()
    ipcs = Path(args.ipcs).expanduser().resolve()
    if not (ipcs / "Makefile").is_file() or not (ipcs / "snappy/snappy.c").is_file():
        parser.error(f"{ipcs} is not the Project2/ipcs directory")
    if Path("/tmp/tinyfile_server_info.txt").exists():
        parser.error("a TinyFile server appears to be running; stop it before starting this test")
    print("Building project...", flush=True)
    subprocess.run(["make", "build"], cwd=ipcs, check=True, stdout=subprocess.DEVNULL)
    with tempfile.TemporaryDirectory(prefix="tinyfile_tests_") as dirname:
        temp = Path(dirname)
        lib = build_decoder(ipcs, temp)
        input_dir = ipcs.parent / "bin/input"
        if not (input_dir / "Tiny.txt").is_file():
            parser.error(f"sample inputs not found in {input_dir}")
        tiny = input_dir / "Tiny.txt"
        test_cases = [(1, 32), (3, 256), (5, 4096)]
        if args.matrix:
            test_cases = [(n, size) for n in (1, 3, 5)
                          for size in (32, 64, 128, 256, 512, 1024, 2048, 4096, 8192)]
        passed = 0
        failed = 0
        for n, size in test_cases:
            for mode in ("SYNC", "ASYNC"):
                label = f"single_n{n}_size{size}_{mode}"
                try:
                    error = test_server(ipcs, lib, temp, n, size, mode, [tiny], label)
                except Exception as e:
                    error = str(e)
                if error:
                    failed += 1
                    print(f"{label}  FAIL: {error}", flush=True)
                else:
                    passed += 1
                    print(f"{label}  PASS", flush=True)
        # Multi-file test: one client, multiple file types.
        inputs = [input_dir / name for name in ("Tiny.txt", "Medium.pdf", "Huge.jpg")]
        if all(f.is_file() for f in inputs):
            for mode in ("SYNC", "ASYNC"):
                label = "multifile_" + mode
                try:
                    error = test_server(ipcs, lib, temp, 5, 4096, mode, inputs, label)
                except Exception as e:
                    error = str(e)
                if error:
                    failed += 1
                    print(f"{label}  FAIL: {error}", flush=True)
                else:
                    passed += 1
                    print(f"{label}  PASS", flush=True)
        print(f"\nRESULT: {passed} passed, {failed} failed")
        if failed:
            print("A passing client exit status is NOT sufficient; outputs must decode to exact input bytes.")
        return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
