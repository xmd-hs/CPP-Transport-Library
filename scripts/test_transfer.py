#!/usr/bin/env python3
"""File transfer regression: upload/resume/download/range/parallel/empty/auth."""
import argparse
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, default=Path(__file__).resolve().parents[1] / 'example')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=19392)
    args = parser.parse_args()
    binaries = args.bin_dir.resolve()
    with tempfile.TemporaryDirectory(prefix='muduo-regression-') as tmp:
        work = Path(tmp)
        root = work / 'root'
        source = work / 'input.bin'
        data = bytes(range(251)) * 25000  # non-aligned, > multiple upload batches
        source.write_bytes(data)
        with (work / 'server.log').open('wb') as log:
            server = subprocess.Popen([str(binaries / 'cloud_disk_server'), str(args.port),
                                       str(root), '2', 'test-token'], stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 10
                while True:
                    if server.poll() is not None:
                        raise RuntimeError('server exited during startup')
                    try:
                        with socket.create_connection((args.host, args.port), timeout=.2):
                            break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(.05)

                def client(mode, *paths, success=True):
                    result = subprocess.run([str(binaries / 'cloud_disk_client'), mode,
                                             args.host, str(args.port)] + list(map(str, paths)),
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
                    if (result.returncode == 0) != success:
                        raise RuntimeError(result.stdout.decode(errors='replace'))

                client('upload', source, 'docs/file.bin', 0, 'test-token')
                assert (root / 'docs/file.bin').read_bytes() == data
                (root / 'docs/file.bin').write_bytes(data[:123457])
                client('upload', source, 'docs/file.bin', 123457, 'test-token')
                assert (root / 'docs/file.bin').read_bytes() == data
                out = work / 'out.bin'
                client('download', 'docs/file.bin', out, 0, 0, 'test-token')
                assert out.read_bytes() == data
                out.write_bytes(data[:98765])
                client('download', 'docs/file.bin', out, 98765, 0, 'test-token')
                assert out.read_bytes() == data
                # Explicit first range must not destroy a concurrently written tail.
                out.write_bytes(b'?' * 100 + data[100:])
                client('download', 'docs/file.bin', out, 0, 100, 'test-token')
                assert out.read_bytes() == data
                for _ in range(3):
                    client('pdownload', 'docs/file.bin', out, 4, 'test-token')
                    assert out.read_bytes() == data
                empty = work / 'empty.bin'
                empty.write_bytes(b'')
                client('upload', empty, 'empty.bin', 0, 'test-token')
                client('download', 'empty.bin', out, 0, 0, 'test-token')
                assert out.read_bytes() == b''
                client('upload', source, 'denied.bin', 0, 'wrong-token', success=False)
                assert not (root / 'denied.bin').exists()
                print('PASS: upload, resume, download, range, parallel x3, empty, rejected auth')
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()


if __name__ == '__main__':
    main()
