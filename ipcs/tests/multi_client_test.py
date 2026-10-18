#!/usr/bin/env python3
"""Concurrent TinyFile integration test: correctness, CST, overlap and QoS diagnostics.

Run from Project2/ipcs:
  python3 tests/multi_client_test.py --mode MIXED --weights 3,2,1
  python3 tests/multi_client_test.py --mode ASYNC --weights 3,2,1 --jobs 12

The script starts/stops its own server. Stop any existing server first.
"""

import argparse
from collections import Counter, defaultdict
import ctypes
import json
import os
from pathlib import Path
import random
import re
import signal
import subprocess
import sys
import time
from datetime import datetime

INFO_FILE = Path('/tmp/tinyfile_server_info.txt')
CST_RE = re.compile(r'^TINYFILE_CST\|file=([^|]+)\|mode=(SYNC|ASYNC)\|seconds=([0-9]+(?:\.[0-9]+)?)$')
TRACE_RE = re.compile(r'^QOS\|stage=(grant|compress|send)\|client=(\d+)\|weight=(\d+)\|req=(\d+)$')
DECISION_RE = re.compile(r'^QOSDEC\|stage=(grant|compress|send)\|mask=(\d+)\|client=(\d+)\|weight=(\d+)')


def build_decoder(ipcs: Path, output: Path):
    libpath = output / 'libsnappy_verify.so'
    subprocess.run([
        'gcc', '-std=gnu11', '-O2', '-fPIC', '-shared',
        '-I', str(ipcs / 'snappy'), str(ipcs / 'snappy/snappy.c'),
        '-o', str(libpath)
    ], check=True)
    lib = ctypes.CDLL(str(libpath))
    lib.snappy_uncompressed_length.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    lib.snappy_uncompressed_length.restype = ctypes.c_bool
    lib.snappy_uncompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    lib.snappy_uncompress.restype = ctypes.c_int
    return lib


def check_snappy(lib, original: Path, result: Path):
    if not result.is_file():
        return 'missing .snappy output'
    expected = original.read_bytes()
    encoded = result.read_bytes()
    if not encoded:
        return 'empty output'
    src = ctypes.create_string_buffer(encoded, len(encoded))
    length = ctypes.c_size_t()
    if not lib.snappy_uncompressed_length(src, len(encoded), ctypes.byref(length)):
        return 'invalid Snappy length/header'
    if length.value != len(expected):
        return f'decompressed size {length.value}, expected {len(expected)}'
    decoded = ctypes.create_string_buffer(max(length.value, 1))
    code = lib.snappy_uncompress(src, len(encoded), decoded)
    if code != 0:
        return f'Snappy decoder returned {code}'
    if decoded.raw[:length.value] != expected:
        return 'decompressed bytes differ from original input'
    return None


def make_inputs(root: Path, client_idx: int, jobs: int, size: int):
    input_dir = root / f'client_{client_idx}' / 'inputs'
    input_dir.mkdir(parents=True)
    rng = random.Random(12345 + client_idx)
    originals = []
    for job in range(jobs):
        # Mix binary/noncompressible and repetitive/compressible inputs.
        # All names are unique, so output files cannot overwrite each other.
        name = f'client{client_idx}_job{job:03d}.bin'
        path = input_dir / name
        if job % 2 == 0:
            data = rng.randbytes(size)
        else:
            pattern = f'client={client_idx};job={job};TinyFile!'.encode()
            data = (pattern * ((size + len(pattern) - 1) // len(pattern)))[:size]
        path.write_bytes(data)
        originals.append(path)
    listing = root / f'client_{client_idx}' / 'files.txt'
    listing.write_text(''.join(str(p) + '\n' for p in originals))
    return originals, listing


def wait_for_server(server, logpath, timeout=8.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if server.poll() is not None:
            raise RuntimeError(f'Server exited with {server.returncode}: {logpath.read_text(errors="replace")[-1200:]}')
        if logpath.exists() and 'TINYFILE_READY\n' in logpath.read_text(errors='replace'):
            return
        time.sleep(0.05)
    raise RuntimeError('Server did not report TINYFILE_READY')


def stop_process(p, *, grace=4):
    if p.poll() is not None:
        return
    try:
        os.killpg(p.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        p.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()


def analyse_qos(logfile: Path, weights):
    lines = logfile.read_text(errors='replace').splitlines()
    counts = defaultdict(Counter)
    by_weight = defaultdict(Counter)
    weight_for_client = {}
    decisions = defaultdict(Counter)
    all_mask = (1 << len(weights)) - 1
    for line in lines:
        m = TRACE_RE.match(line)
        if m:
            stage, client, weight, req = m.groups()
            client, weight = int(client), int(weight)
            counts[stage][(client, weight)] += 1
            by_weight[stage][weight] += 1
            weight_for_client[client] = weight
        m = DECISION_RE.match(line)
        if m:
            stage, mask, client, weight = m.groups()
            if int(mask) == all_mask:
                decisions[stage][int(weight)] += 1
    return counts, by_weight, weight_for_client, decisions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ipcs', type=Path, default=Path.cwd(), help='Path to Project2/ipcs (default: current dir)')
    parser.add_argument('--weights', default='3,2,1', help='Comma-separated weights, one client per weight')
    parser.add_argument('--mode', choices=('SYNC', 'ASYNC', 'MIXED'), default='MIXED')
    parser.add_argument('--jobs', type=int, default=8, help='Number of unique files per client')
    parser.add_argument('--bytes', type=int, default=16384, help='Input size per file')
    parser.add_argument('--n-sms', type=int, default=5)
    parser.add_argument('--sms-size', type=int, default=4096)
    parser.add_argument('--timeout', type=float, default=45, help='Maximum client runtime in seconds')
    parser.add_argument('--no-build', action='store_true')
    parser.add_argument('--start-delay-ms', type=int, default=0,
                        help='Optional test-only start barrier; requires TINYFILE_TEST_START_MS support in sample app')
    parser.add_argument('--decision-trace', action='store_true',
                        help='Set TINYFILE_QOS_DECISIONS=1 for optional QOSDEC instrumentation')
    parser.add_argument('--require-contention', action='store_true',
                        help='Fail if fewer than 30 grant/send decisions have all clients eligible')
    parser.add_argument('--out', type=Path, help='Directory for generated inputs, outputs and logs')
    args = parser.parse_args()

    try:
        weights = [int(w.strip()) for w in args.weights.split(',')]
    except ValueError:
        parser.error('--weights must be comma-separated integers')
    if not 2 <= len(weights) <= 8 or any(w <= 0 for w in weights):
        parser.error('Provide 2-8 positive client weights')
    if args.jobs < 1 or args.bytes < 1 or not (1 <= args.n_sms <= 5) or args.sms_size < 1 or args.timeout <= 0 or args.start_delay_ms < 0:
        parser.error('jobs, bytes, timeout, sms-size must be positive; n-sms must be 1-5')

    ipcs = args.ipcs.resolve()
    if not (ipcs / 'Makefile').exists() or not (ipcs / 'snappy/snappy.c').exists():
        parser.error(f'Not a TinyFile ipcs directory: {ipcs}')
    if INFO_FILE.exists():
        parser.error(f'{INFO_FILE} already exists: stop the existing server first')

    if not args.no_build:
        print('Building TinyFile ...', flush=True)
        subprocess.run(['make', 'build'], cwd=ipcs, check=True)

    stamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    root = (args.out or ipcs / 'test_runs' / f'multiclient_{stamp}').resolve()
    root.mkdir(parents=True, exist_ok=True)
    lib = build_decoder(ipcs, root)
    print(f'Run directory: {root}', flush=True)

    specs = []
    for i, weight in enumerate(weights):
        files, listing = make_inputs(root, i, args.jobs, args.bytes)
        mode = args.mode if args.mode != 'MIXED' else ('SYNC' if i % 2 == 0 else 'ASYNC')
        outdir = root / f'client_{i}' / 'outputs'
        outdir.mkdir(parents=True, exist_ok=True)
        specs.append(dict(index=i, weight=weight, files=files, listing=listing, mode=mode,
                          outdir=outdir, stdout=root / f'client_{i}.log'))

    server_log = root / 'server.log'
    server = None
    clients = []
    errors = []
    try:
        with server_log.open('w') as slog:
            server_env = os.environ.copy()
            if args.decision_trace:
                server_env['TINYFILE_QOS_DECISIONS'] = '1'
            server = subprocess.Popen(['./run-server.sh', '--n_sms', str(args.n_sms),
                                       '--sms_size', str(args.sms_size)], cwd=ipcs,
                                      env=server_env, stdout=slog, stderr=subprocess.STDOUT,
                                      start_new_session=True)
            wait_for_server(server, server_log)
            print(f'Server ready. Launching {len(specs)} clients ...', flush=True)
            client_env = os.environ.copy()
            if args.start_delay_ms:
                client_env['TINYFILE_TEST_START_MS'] = str(int(time.time() * 1000) + args.start_delay_ms)
            # Launch back-to-back: real OS processes overlap; large multi-file
            # workloads keep clients busy for scheduling contention.
            for spec in specs:
                handle = spec['stdout'].open('w')
                command = ['./run-client.sh', '--files', str(spec['listing']), '--state', spec['mode'],
                           '--freqs', str(spec['weight']), '--output_dir', str(spec['outdir'])]
                p = subprocess.Popen(command, cwd=ipcs, env=client_env, stdout=handle,
                                     stderr=subprocess.STDOUT, start_new_session=True)
                spec['process'] = p
                spec['handle'] = handle
                spec['started'] = time.monotonic()
                clients.append(spec)
            deadline = time.monotonic() + args.timeout
            while any(spec['process'].poll() is None for spec in clients):
                if server.poll() is not None:
                    errors.append(f'Server exited during workload (exit {server.returncode})')
                    break
                if time.monotonic() >= deadline:
                    errors.append(f'Client workload exceeded timeout {args.timeout}s')
                    break
                time.sleep(0.02)
    finally:
        for spec in clients:
            stop_process(spec['process'])
            spec['ended'] = time.monotonic()
            spec['handle'].close()
        if server is not None:
            stop_process(server)

    for spec in clients:
        idx = spec['index']
        log = spec['stdout'].read_text(errors='replace')
        lines = log.splitlines()
        cst = [m for line in lines if (m := CST_RE.fullmatch(line))]
        names = [m.group(1) for m in cst]
        expected_names = [f.name for f in spec['files']]
        if spec['process'].returncode != 0:
            errors.append(f'Client {idx} (weight={spec["weight"]}, mode={spec["mode"]}) exited {spec["process"].returncode}: {log[-350:]}')
        if len(cst) != len(spec['files']) or Counter(names) != Counter(expected_names):
            errors.append(f'Client {idx}: expected {len(spec["files"])} unique CST records, got {len(cst)} (names={names})')
        if any(m.group(2) != spec['mode'] for m in cst):
            errors.append(f'Client {idx}: wrong CST transfer mode')
        for f in spec['files']:
            err = check_snappy(lib, f, spec['outdir'] / (f.name + '.snappy'))
            if err:
                errors.append(f'Client {idx}, file {f.name}: {err}')
        timings = [float(m.group(3)) for m in cst]
        avg = (sum(timings) / len(timings)) if timings else float('nan')
        print(f'Client {idx}: weight={spec["weight"]:<2} mode={spec["mode"]:<5} '
              f'completed={len(cst)}/{args.jobs} avg_CST={avg:.4f}s '
              f'exit={spec["process"].returncode}')

    if clients:
        overlapping = max(s['started'] for s in clients) < min(s['ended'] for s in clients)
        print(f'Client process lifetimes overlap: {overlapping}')
        if not overlapping:
            errors.append('Client process lifetimes did not overlap; increase --jobs/--bytes')

    counts, by_weight, assignment, contended = analyse_qos(server_log, weights)
    print('\nQoS selections (totals across whole run; NOT guaranteed to reflect contention):')
    for stage in ('grant', 'compress', 'send'):
        summary = ', '.join(f'slot{slot}(w{weight})={n}'
                            for (slot, weight), n in sorted(counts[stage].items()))
        print(f'  {stage:<8} {summary or "no traces found"}')
        if contended[stage]:
            total = sum(contended[stage].values())
            expected = sum(weights)
            print(f'    all-client eligible decisions: {total}')
            for weight in sorted(set(weights), reverse=True):
                count = contended[stage][weight]
                share = 100 * count / total
                target = 100 * weight * weights.count(weight) / expected
                print(f'      weight={weight}: {count} ({share:.1f}%), expected about {target:.1f}%')

    if not any(contended.values()):
        print('NOTE: No QOSDEC eligibility masks found; total counts do NOT prove 3:2:1 fairness.')
        print('      To measure fairness, log each scheduling decision with its eligible-client mask.')
    if args.require_contention and sum(sum(contended[s].values()) for s in ('grant', 'send')) < 30:
        errors.append('Insufficient simultaneous eligibility for QoS testing (need >=30 grant/send QOSDEC events); add the opt-in client start barrier and server QOSDEC logging')
    if INFO_FILE.exists():
        errors.append(f'Server info file remains after shutdown: {INFO_FILE}')
    if server is not None and server.returncode != 0:
        errors.append(f'Server exited with code {server.returncode}')

    report = {
        'weights': weights, 'mode': args.mode, 'jobs_per_client': args.jobs,
        'n_sms': args.n_sms, 'sms_size': args.sms_size,
        'clients': [dict(index=s['index'], weight=s['weight'], mode=s['mode'],
                         exit_code=s['process'].returncode) for s in clients],
        'errors': errors
    }
    (root / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'\nLogs and outputs: {root}')
    if errors:
        print(f'FAIL: {len(errors)} problem(s):')
        for error in errors[:20]:
            print(f' - {error}')
        if len(errors) > 20:
            print(f' - ... and {len(errors) - 20} more')
        return 1
    print('PASS: all clients completed, every file round-tripped through Snappy, server cleaned up')
    return 0


if __name__ == '__main__':
    sys.exit(main())
