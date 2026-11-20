#!/usr/bin/env python3
"""Exercise the standalone C SDK against the Linux CLDT server."""
import argparse
import hashlib
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.digest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--sdk-sample', type=Path, required=True)
    parser.add_argument('--sdk-memory-sample', type=Path,
                        help='Sample that exercises custom file I/O callbacks')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=19407)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix='cldt-sdk-test-') as directory:
        work = Path(directory)
        root = work / 'root'
        source = work / 'source.bin'
        downloaded = work / 'download.bin'
        source.write_bytes(bytes(range(251)) * 4096 + b'CLDT SDK test')
        with (work / 'server.log').open('wb') as log:
            server = subprocess.Popen(
                [str(args.server.resolve()), str(args.port), str(root), '1', 'sdk-token'],
                stdout=log, stderr=subprocess.STDOUT,
                env=dict(os.environ, CLDT_BIND_IP=args.host))
            try:
                deadline = time.monotonic() + 10
                while True:
                    if server.poll() is not None:
                        raise RuntimeError('server stopped during startup')
                    try:
                        with socket.create_connection((args.host, args.port), timeout=.2):
                            break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise RuntimeError('server did not start')
                        time.sleep(.05)

                sample = str(args.sdk_sample.resolve())
                subprocess.run([sample, args.host, str(args.port), 'sdk-token',
                                'upload', str(source), 'sdk/data.bin'],
                               check=True, timeout=30)
                subprocess.run([sample, args.host, str(args.port), 'sdk-token',
                                'stat', 'sdk/data.bin'], check=True, timeout=30)
                subprocess.run([sample, args.host, str(args.port), 'sdk-token',
                                'download', 'sdk/data.bin', str(downloaded)],
                               check=True, timeout=30)
                if digest(source) != digest(downloaded):
                    raise RuntimeError('SDK round-trip SHA256 mismatch')
                # Resume a non-aligned upload and download, and preserve the
                # tail when filling an explicit first range.
                data = source.read_bytes()
                (root / 'sdk/data.bin').write_bytes(data[:123457])
                subprocess.run([sample, args.host, str(args.port), 'sdk-token',
                                'upload', str(source), 'sdk/data.bin', '123457'],
                               check=True, timeout=30)
                if digest(source) != digest(root / 'sdk/data.bin'):
                    raise RuntimeError('SDK resumed upload mismatch')
                downloaded.write_bytes(data[:98765])
                subprocess.run([sample, args.host, str(args.port), 'sdk-token',
                                'download', 'sdk/data.bin', str(downloaded), '98765'],
                               check=True, timeout=30)
                if digest(source) != digest(downloaded):
                    raise RuntimeError('SDK resumed download mismatch')
                downloaded.write_bytes(b'?' * 101 + data[101:])
                subprocess.run([sample, args.host, str(args.port), 'sdk-token',
                                'download', 'sdk/data.bin', str(downloaded), '0', '101'],
                               check=True, timeout=30)
                if digest(source) != digest(downloaded):
                    raise RuntimeError('SDK range download changed tail')
                empty = work / 'empty.bin'
                empty.write_bytes(b'')
                subprocess.run([sample, args.host, str(args.port), 'sdk-token',
                                'upload', str(empty), 'sdk/empty.bin'],
                               check=True, timeout=30)
                subprocess.run([sample, args.host, str(args.port), 'sdk-token',
                                'download', 'sdk/empty.bin', str(downloaded)],
                               check=True, timeout=30)
                if downloaded.read_bytes() != b'':
                    raise RuntimeError('SDK empty download did not truncate')
                denied = subprocess.run([sample, args.host, str(args.port), 'wrong-token',
                                         'upload', str(source), 'sdk/denied.bin'],
                                        timeout=30, capture_output=True)
                if denied.returncode == 0 or (root / 'sdk/denied.bin').exists():
                    raise RuntimeError('SDK accepted invalid authentication')
                # A peer closing during sendfile must report failure rather
                # than kill the host application with SIGPIPE.
                broken_source = work / 'broken-source.bin'
                with broken_source.open('wb') as stream:
                    stream.truncate(32 * 1024 * 1024)
                with socket.socket() as listener:
                    listener.bind((args.host, 0))
                    listener.listen(1)
                    listener.settimeout(10)

                    def close_peer():
                        try:
                            connection, _ = listener.accept()
                            with connection:
                                connection.recv(40)
                                connection.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass

                    peer = threading.Thread(target=close_peer)
                    peer.start()
                    try:
                        disconnected = subprocess.run(
                            [sample, args.host, str(listener.getsockname()[1]),
                             'sdk-token', 'upload', str(broken_source), 'broken.bin'],
                            timeout=20, capture_output=True)
                        if disconnected.returncode != 2:
                            raise RuntimeError('SDK disconnect did not report failure: ' +
                                               str(disconnected.returncode))
                    finally:
                        peer.join(timeout=11)
                if args.sdk_memory_sample:
                    subprocess.run([str(args.sdk_memory_sample.resolve()), args.host,
                                    str(args.port), 'sdk-token', 'sdk/memory.bin'],
                                   check=True, timeout=30)
                print('PASS: SDK upload, stat, download, resume, range, empty, auth, disconnect, SHA256' +
                      (' and custom file I/O' if args.sdk_memory_sample else ''))
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()


if __name__ == '__main__':
    main()
