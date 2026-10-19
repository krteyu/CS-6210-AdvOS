#!/usr/bin/env python3
"""Concurrent TinyFile integration test: correctness, CST, overlap and QoS diagnostics.

Run from Project2/ipcs:
  python3 tests/multi_client_test.py --mode MIXED --weights 3,2,1 --n_clients 3
  python3 tests/multi_client_test.py --mode ASYNC --weights 3,2,1,3 --n_clients 4 

The script starts/stops its own server. Stop any existing server first.
"""

import subprocess
import argparse
import ctypes
import re
import time
import os
import signal
import csv
from datetime import datetime
from dataclasses import dataclass
from pathlib import Path
from collections import Counter

INFO_FILE = Path('/tmp/tinyfile_server_info.txt')
CST_RE = re.compile(r'^TINYFILE_CST\|file=([^|]+)\|mode=(SYNC|ASYNC)\|seconds=([0-9]+(?:\.[0-9]+)?)$')
TRACE_RE = re.compile(
    r'^QOS\|stage=(grant|compress|send)'
    r'\|client=(\d+)'
    r'\|weight=(\d+)'
    r'\|req=(\d+)'
    r'(?:\|mask=(\d+))?$'
)

@dataclass
class ClientSpec:
    index: int
    weight: int
    mode: str
    files: list[Path]
    file_list: Path
    output_dir: Path
    log_path: Path

    process: subprocess.Popen | None = None
    started_at: float | None = None
    ended_at: float | None = None

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

def parse_arguments() -> argparse.Namespace:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument('--ipcs', type=Path, default=Path.cwd(), help='Path to Project2/ipcs (default: current dir)')
	parser.add_argument('--input-dir', type=Path, default=None, help='Directory containing the five project input files')
	parser.add_argument('--n_clients', type=int, default=3, help='Number of concurrent clients')
	parser.add_argument('--weights', default='3,2,1', help='Comma-separated weights, one client per weight')
	parser.add_argument('--mode', choices=('SYNC', 'ASYNC', 'MIXED'), default='MIXED')
	parser.add_argument('--n-sms', type=int, default=5)
	parser.add_argument('--sms-size', type=int, default=4096)
	parser.add_argument('--timeout', type=float, default=45, help='Maximum client runtime in seconds')
	parser.add_argument('--out', type=Path, help='Directory for generated inputs, outputs and logs')
	args = parser.parse_args()

	try:
		args.weights = [
		int(w.strip()) for w in args.weights.split(',')
		]
	except ValueError:
		parser.error('weights must be a comma-separated list of integers')

	if len(args.weights) != args.n_clients:
		parser.error(f'Expected {args.n_clients} weights, got {len(args.weights)} instead')

	if any(w <= 0 for w in args.weights):
		parser.error("All weights must be positive")

	if not 2 <= args.n_clients <= 8:
		parser.error("Number of clients must be between 2 and 8")

	if args.n_sms <= 0 or args.sms_size <= 0:
		parser.error("Segment count and size must be positive")
	
	if args.n_sms > 5 or args.sms_size > 8192:
		parser.error("Segment count must be less than or equal to 5 and/or segment size must be under 8192 bytes")

	if args.timeout <= 0:
		parser.error("Invalid timeout")

	return args

def prepare_dirs(ipcs: Path, config) -> Path:
    if not (ipcs / "Makefile").exists():
        raise ValueError(f"Not a TinyFile directory: {ipcs}")

    if not (ipcs / "snappy/snappy.c").exists():
        raise ValueError("Snappy source not found")

    if INFO_FILE.exists():
        raise RuntimeError("An existing TinyFile server may be running")

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")

    root = (
        config.out if config.out is not None
        else ipcs / "test_runs" / f"multiclient_{stamp}_{config.sms_size}"
    ).resolve()

    root.mkdir(parents=True, exist_ok=True)

    return root

def prepare_client_inputs(
    input_dir: Path,
    client_dir: Path) -> tuple[list[Path], Path]:

	if not input_dir.is_dir():
		raise FileNotFoundError(input_dir)
	files = sorted(input_dir.iterdir())
	files = [f for f in files if f.is_file()]
	client_job_listing = client_dir / 'files.txt'
	client_job_listing.write_text('\n'.join(str(f.resolve()) for f in files ) + '\n')
	return files, client_job_listing

def prepare_clients(config, input_dir, root):
	clients = []
	for i, weight in enumerate(config.weights):
		client_dir = (root / f"client_{i}").resolve()
		client_dir.mkdir(parents=True, exist_ok=True)
		files, file_list = prepare_client_inputs(input_dir, client_dir)
		mode = config.mode if (config.mode != 'MIXED') else ('SYNC' if i%2 else 'ASYNC')
		output_dir = client_dir / 'output'
		output_dir.mkdir(parents=True, exist_ok=True)
		log_path = client_dir / f"client{i}.log"
		client = ClientSpec(
			index=i,
			weight=weight,
			mode=mode, 
			files=files,
			file_list=file_list,
			output_dir=output_dir,
			log_path=log_path
			)
		clients.append(client)
	return clients

def stop_process(process: subprocess.Popen, grace: float = 4.0) -> None:
	if process.poll() is not None:
		return
	
	try:
		os.killpg(process.pid, signal.SIGTERM)
		process.wait(timeout=grace)
	
	except ProcessLookupError:
		return
	
	except subprocess.TimeoutExpired:
		os.killpg(process.pid, signal.SIGKILL)
		process.wait()

def wait_for_server(
    server: subprocess.Popen,
    log_path: Path,
    timeout: float = 8.0) -> None:

	deadline = time.monotonic() + timeout
	while time.monotonic() < deadline:
		if server.poll() is not None:
			raise RuntimeError("Server exited before becoming ready")
		if log_path.exists():
			content = log_path.read_text(errors="replace")
			if "TINYFILE_READY" in content:
				return
		time.sleep(0.05)
	raise TimeoutError(f"Server did not become ready within {timeout} seconds")
	
def start_server(
    ipcs: Path,
    config: argparse.Namespace,
    log_path: Path) -> subprocess.Popen:

	command = ["./run-server.sh", 
	"--n_sms", str(config.n_sms), 
	"--sms_size", str(config.sms_size)]
	with log_path.open('w') as server_log:
		server = subprocess.Popen(command, stdout=server_log, stderr=subprocess.STDOUT, cwd=ipcs, start_new_session=True)
		try:
			wait_for_server(server, log_path)
		except Exception:
			stop_process(server)
			raise
	return server

def wait_for_clients(
    clients: list[ClientSpec],
    server: subprocess.Popen,
    timeout: float) -> None:
	deadline = time.monotonic() + timeout
	while True:
		all_finished = True
		for client in clients:
			if client.ended_at is None:
				if client.process.poll() is not None:
					client.ended_at = time.monotonic()
				else:
					all_finished= False
		if all_finished:
			break
		if(server.poll() is not None):
			raise RuntimeError('Server terminated')
		if (time.monotonic() >= deadline):
			raise TimeoutError("Some clients have timed out")
		time.sleep(0.02)

def verify_client_execution(client: ClientSpec) -> list[str]:
	errors = []
	lines = client.log_path.read_text().splitlines()
	matches = []
	for line in lines:
		match = CST_RE.fullmatch(line)
		if match:
			matches.append(match)

	if client.process.returncode != 0:
		errors.append(f"Client {client.index} exited with code {client.process.returncode}")

	expected = Counter(f.name for f in client.files)
	observed = Counter(m.group(1) for m in matches)
	if expected != observed:
		errors.append("CST filenames do not match expected files")

	for match in matches:
		if match.group(2) != client.mode:
			errors.append("Unexpected transfer mode")
	return errors
	
def launch_clients(
    ipcs: Path,
    clients: list[ClientSpec]) -> None:
	for client in clients:
		command = ["./run-client.sh",
		"--files",str(client.file_list),
		"--state",client.mode,
		"--freqs",str(client.weight),
		"--output_dir",str(client.output_dir)]

		with client.log_path.open("w") as log:
			client.process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, cwd=ipcs, start_new_session=True)
			client.started_at = time.monotonic()

def verify_snappy_outputs(
    client: ClientSpec,
    decoder) -> list[str]:
	errors = []
	for original in client.files:
		output = (client.output_dir / f"{original.name}.snappy").resolve()
		if not output.exists():
			errors.append(f"[{client.index}:]File {original.name}.snappy doesn't exist")
			continue

		encoded = output.read_bytes()
		original_bytes = original.read_bytes()

		source_buffer = ctypes.create_string_buffer(encoded, len(encoded))
		decoded_length = ctypes.c_size_t()
		valid = decoder.snappy_uncompressed_length(source_buffer, len(encoded), ctypes.byref(decoded_length))
		if not valid:
			errors.append(f"[{client.index}:] Invalid Snappy data: {output.name}")
			continue

		if decoded_length.value != len(original_bytes):
			errors.append(f"[{client.index}:] Unexpected decompressed size: {output.name}")
			continue

		destination = ctypes.create_string_buffer(max(decoded_length.value, 1))	
		rc = decoder.snappy_uncompress(source_buffer, len(encoded), destination)
		if rc != 0:
			errors.append(f"[{client.index}:] Snappy decompression failed: {output.name}")

		if destination.raw[:decoded_length.value] != original_bytes:
			errors.append(f"[{client.index}:]Content mismatch {original.name}")
	return errors

def analyse_qos(
	log_path: Path,
	clients: list[ClientSpec]) -> dict:
	stages = ("grant", "compress", "send")
	observed_weights = {}
	n_clients = len(clients)
	all_mask = (1 << n_clients) - 1

	total = {stage: Counter() for stage in stages}
	fully_contended = {stage: Counter() for stage in stages}
	partial_contention = {stage: 0 for stage in stages}

	missing_masks = 0
	invalid_masks = 0

	with log_path.open("r", errors="replace") as log:
		for line in log:
			match = TRACE_RE.fullmatch(line.strip())

			if match is None:
				continue
			
			stage, client_str, weight_str, req_str, mask_str = match.groups()

			client_id = int(client_str)
			weight = int(weight_str)
			observed_weights.setdefault(client_id, set()).add(weight)
			mask = int(mask_str) if mask_str is not None else None

			total[stage][client_id] += 1

			if mask is None:
				missing_masks += 1
				continue
			
			if mask & ~all_mask:
				invalid_masks += 1
				continue
			
			if mask == all_mask:
				fully_contended[stage][client_id] += 1
			
			elif mask.bit_count() >= 2:
				partial_contention[stage] += 1
	
	return{
		"total": total,
		"fully_contended": fully_contended,
		"partial_contention": partial_contention,
		"missing_masks": missing_masks,
		"invalid_masks": invalid_masks,
		"observed_weights": observed_weights
	}

def print_qos_summary(qos: dict, clients: list[ClientSpec]) -> None:
	weights = {}
	for client_id, observed in qos["observed_weights"].items():
		if len(observed) != 1:
			raise RuntimeError(
				f"Server slot {client_id} has inconsistent weights: {observed}"
			)
		weights[client_id] = next(iter(observed))
	
	weight_sum = sum(weights.values())

	for stage in ("grant", "compress", "send"):
		total = qos["total"][stage]
		full = qos["fully_contended"][stage]

		total_decisions = sum(total.values())
		full_decisions = sum(full.values())

		print(f"\nQoS stage: {stage}")
		print(f"  Total decisions: {total_decisions}")
		print(f"  Fully contended: {full_decisions}")
		print(f"  Partially contended: {qos['partial_contention'][stage]}")

		for client_id, weight in weights.items():
			expected = 100 * weight / weight_sum
			overall_share = (100 * total[client_id] / total_decisions if total_decisions else 0.0)
			full_share = (100 * full[client_id] / full_decisions if full_decisions else 0.0)

			print(f"  Client {client_id}: "
				f"total={total[client_id]}, "
				f"overall={overall_share:.1f}%, "
				f"fully-contended={full_share:.1f}%, "
				f"expected={expected:.1f}%")

	for client_id, weights in qos["observed_weights"].items():
		print(f"Server slot {client_id}: weights={weights}")
	print(f"\nMissing masks: {qos['missing_masks']}")
	print(f"Invalid masks: {qos['invalid_masks']}")

def print_client_timings(clients: list[ClientSpec]) -> None:
	print("\nClient execution times:")

	for client in clients:
		if client.started_at is None or client.ended_at is None:
			print(f"Client {client.index}: timing unavailable")
			continue

		duration = client.ended_at - client.started_at

		print(
			f"Client {client.index}: "
			f"weight={client.weight}, "
			f"mode={client.mode}, "
			f"time={duration:.4f}s"
		)

def save_cst_csv(clients, segment_size, n_sms, output_path):
	with output_path.open("w", newline="") as f:
		writer = csv.writer(f)
		writer.writerow(["segment_size", "n_sms", "client", "weight","mode", "file", "cst_seconds"])
		for client in clients:
			for line in client.log_path.open():
				match = CST_RE.fullmatch(line.strip())
				if match:
					writer.writerow([
						segment_size,
						n_sms,
						client.index,
						client.weight,
						match.group(2),
						match.group(1),
						float(match.group(3))
					])

def main():
	config = parse_arguments()
	ipcs = config.ipcs.resolve()
	root = prepare_dirs(ipcs, config)
	lib = build_decoder(ipcs, root)
	input_dir = (config.input_dir.resolve() if config.input_dir is not None else ipcs.parent / "bin" / "input")
	clients = prepare_clients(config, input_dir, root)
	for client in clients:
		print(f"Client {client.index}: ",f"weight={client.weight}, ",f"mode={client.mode}, ",f"files={len(client.files)}")
	server_log = root / 'server.log'
	server = None
	try:
		server = start_server(ipcs, config, server_log)
		print(f"Server started: PID={server.pid}")

		launch_clients(ipcs, clients)

		wait_for_clients(clients, server, config.timeout)
		save_cst_csv(clients, config.sms_size, config.n_sms, root / "cst.csv")
		print_client_timings(clients)
		errors = []
		for client in clients:
			errors.extend(verify_client_execution(client))
			errors.extend(verify_snappy_outputs(client, lib))

		if errors:
			for error in errors:
				print(f'FAIL: {error}')
		else:
			print("PASS: all clients completed and outputs verified")
		
		qos = analyse_qos(server_log, clients)
		print_qos_summary(qos, clients)

		if errors:
			raise SystemExit(1)

	finally:
		for client in clients:
			if client.process is not None:
				stop_process(client.process)

		if server is not None:
			stop_process(server)
	

if __name__ == "__main__":
    main()
  
