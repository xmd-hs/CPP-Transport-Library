#!/usr/bin/env python3
"""Local cloud transfer benchmark. Includes process startup and file flush time."""
import argparse
import concurrent.futures
import hashlib
import json
import os
import resource
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def child_cpu():
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return usage.ru_utime + usage.ru_stime


def server_cpu(pid):
    fields = Path('/proc/%d/stat' % pid).read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.digest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'example')
    parser.add_argument('--size-mib', type=int, default=64)
    parser.add_argument('--clients', type=int, default=4)
    parser.add_argument('--port', type=int, default=19390)
    parser.add_argument('--host', default='127.0.0.1', help='Local server address; use the Linux interface IP to compare WSL loopback routing')
    parser.add_argument('--work-dir', type=Path, help='Parent for temporary data, e.g. /dev/shm to exclude disk latency')
    args = parser.parse_args()
    if args.size_mib < 1 or args.clients < 1:
        parser.error('size-mib and clients must be positive')
    binaries = args.bin_dir.resolve()
    with tempfile.TemporaryDirectory(prefix='muduo-bench-', dir=args.work_dir) as tmp:
        work = Path(tmp)
        source = work / 'source.bin'
        # Nonzero deterministic data, generated outside the measured interval.
        block = bytes(range(256)) * 4096
        with source.open('wb') as stream:
            for _ in range(args.size_mib):
                stream.write(block)
        expected = digest(source)
        with (work / 'server.log').open('wb') as log:
            server = subprocess.Popen([str(binaries / 'cloud_disk_server'), str(args.port),
                                       str(work / 'root'), '2', 'bench-token'],
                                      stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 10
                while True:
                    if server.poll() is not None:
                        raise RuntimeError('server exited: ' + (work / 'server.log').read_text())
                    try:
                        with socket.create_connection((args.host, args.port), timeout=.2):
                            break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise RuntimeError('server did not become ready')
                        time.sleep(.05)

                def run(mode, index):
                    remote = 'bench/%d.bin' % index
                    output = work / ('out-%d.bin' % index)
                    paths = [str(source), remote, '0', 'bench-token'] if mode == 'upload' else [
                        remote, str(output), '0', '0', 'bench-token']
                    subprocess.run([str(binaries / 'cloud_disk_client'), mode,
                                    args.host, str(args.port)] + paths,
                                   check=True, timeout=120, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE)

                for mode in ('upload', 'download'):
                    cpu_begin = child_cpu() + server_cpu(server.pid)
                    start = time.perf_counter()
                    with concurrent.futures.ThreadPoolExecutor(max_workers=args.clients) as pool:
                        list(pool.map(lambda i: run(mode, i), range(args.clients)))
                    elapsed = time.perf_counter() - start
                    cpu_used = child_cpu() + server_cpu(server.pid) - cpu_begin
                    for i in range(args.clients):
                        path = work / ('root/bench/%d.bin' % i) if mode == 'upload' else work / ('out-%d.bin' % i)
                        if digest(path) != expected:
                            raise RuntimeError('SHA256 mismatch: ' + str(path))
                    print(json.dumps(dict(mode=mode, clients=args.clients,
                                          host=args.host,
                                          size_mib=args.size_mib, seconds=round(elapsed, 4),
                                          mib_per_second=round(args.clients * args.size_mib / elapsed, 2),
                                          cpu_seconds=round(cpu_used, 4),
                                          sha256='PASS')), flush=True)
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()


if __name__ == '__main__':
    main()
