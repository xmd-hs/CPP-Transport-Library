#!/usr/bin/env python3
"""Same SDK, SYNC completion, real server + stunnel; Linux local benchmark.

Includes handshake, worker startup, file fsync, and gateway/server/client CPU.
Reports warmup separately. Never extrapolate local results to mobile/WAN.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import resource
import statistics
import tempfile
import time
from security_regression import Client, certs, load, services, HOST


def cpu():
    own = resource.getrusage(resource.RUSAGE_SELF)
    children = resource.getrusage(resource.RUSAGE_CHILDREN)
    # Running server/stunnel are not yet in RUSAGE_CHILDREN.
    running = 0
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit(): continue
        try:
            fields = (proc / 'stat').read_text().rsplit(')', 1)[1].split()
            if int(fields[1]) == os.getpid():
                running += (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')
        except (OSError, ValueError): pass
    return own.ru_utime + own.ru_stime + children.ru_utime + children.ru_stime + running


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').digest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, default=Path('/dev/shm'))
    parser.add_argument('--size-mib', type=int, default=256)
    parser.add_argument('--clients', type=int, default=4)
    parser.add_argument('--rounds', type=int, default=3)
    args = parser.parse_args()
    assert min(args.size_mib, args.clients, args.rounds) > 0
    lib, records = load(args.library.resolve()), []
    with tempfile.TemporaryDirectory(prefix='cldt-secure-bench-', dir=args.work_dir) as tmp:
        work = Path(tmp)
        certs(work)
        source = work / 'source'
        with source.open('wb') as stream:
            for _ in range(args.size_mib): stream.write(bytes(range(256)) * 4096)
        expected = digest(source)
        with services(args.server.resolve(), work) as (plain, secure, root):
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.clients) as pool:
                for round_index in range(-1, args.rounds):
                    order = (False, True) if round_index % 2 == 0 else (True, False)
                    for tls in order:
                        for mode in ('upload', 'download'):
                            def transfer(index):
                                target = work / ('out-' + str(index))
                                with Client(lib, secure if tls else plain,
                                            work / 'ca.pem' if tls else None) as client:
                                    client.options.connect_timeout_ms = 10000
                                    client.options.io_timeout_ms = 60000
                                    assert client.configure() == 0
                                    assert client.submit(mode, source if mode == 'upload' else target,
                                                         remote=f'bench-{index}'.encode()) == 0
                                    client.wait()
                            start_cpu, start = cpu(), time.monotonic()
                            list(pool.map(transfer, range(args.clients)))
                            elapsed, used = time.monotonic() - start, cpu() - start_cpu
                            for index in range(args.clients):
                                path = root / f'bench-{index}' if mode == 'upload' else work / f'out-{index}'
                                assert digest(path) == expected
                            record = dict(tls=tls, mode=mode, round=round_index,
                                warmup=round_index < 0, seconds=elapsed, cpu_seconds=used,
                                mib_per_second=args.size_mib * args.clients / elapsed)
                            records.append(record)
                            print(json.dumps(record), flush=True)
    result = dict(host=HOST, clients=args.clients, size_mib=args.size_mib,
                  durability='SYNC', cpu_scope='client + backend + TLS gateway', records=records)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    for tls in (False, True):
        for mode in ('upload', 'download'):
            samples = [r for r in records if r['tls'] == tls and r['mode'] == mode and not r['warmup']]
            print('MEDIAN', tls, mode, statistics.median(r['mib_per_second'] for r in samples),
                  'MiB/s, CPU', statistics.median(r['cpu_seconds'] for r in samples), flush=True)


if __name__ == '__main__': main()
