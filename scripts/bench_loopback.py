#!/usr/bin/env python3
"""Independent TCP baseline: no project library, allocator or file I/O."""
import argparse
import concurrent.futures
import json
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--size-mib', type=int, default=64)
    parser.add_argument('--clients', type=int, default=1)
    parser.add_argument('--port', type=int, default=19391)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--chunk-kib', type=int, default=1024)
    parser.add_argument('--socket-buffer', type=int, default=0,
                        help='Set SO_SNDBUF/SO_RCVBUF on both endpoints; 0 preserves autotuning')
    args = parser.parse_args()
    if args.size_mib <= 0 or args.clients <= 0 or args.chunk_kib <= 0:
        parser.error('size-mib, clients and chunk-kib must be positive')
    size = args.size_mib * 1048576
    block = bytes(range(256)) * (args.chunk_kib * 4)
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('0.0.0.0', args.port))
        listener.listen(args.clients)
        listener.settimeout(.2)
        address = (args.host, args.port)

        def configure(connection):
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            if args.socket_buffer:
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, args.socket_buffer)
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, args.socket_buffer)

        def receive():
            storage = bytearray(1048576)
            deadline = time.monotonic() + 5
            while True:
                try:
                    connection = socket.create_connection(address, timeout=1)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(.01)
            with connection:
                connection.settimeout(60)
                configure(connection)
                count = 0
                while count < size:
                    n = connection.recv_into(storage, min(len(storage), size - count))
                    if not n:
                        raise RuntimeError('unexpected EOF')
                    count += n
                connection.sendall(b'K')
                return count

        def send(connection):
            with connection:
                configure(connection)
                connection.settimeout(60)
                left = size
                while left:
                    chunk = min(len(block), left)
                    connection.sendall(memoryview(block)[:chunk])
                    left -= chunk
                if connection.recv(1) != b'K':
                    raise RuntimeError('missing receive acknowledgement')

        start = time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.clients * 2) as pool:
            readers = [pool.submit(receive) for _ in range(args.clients)]
            writers = []
            while len(writers) < args.clients:
                try:
                    writers.append(pool.submit(send, listener.accept()[0]))
                except socket.timeout:
                    for reader in readers:
                        if reader.done() and reader.exception():
                            reader.result()
            received = sum(r.result() for r in readers)
            for writer in writers:
                writer.result()
        elapsed = time.perf_counter() - start
        print(json.dumps(dict(baseline='python_tcp_no_disk', clients=args.clients,
                              socket_buffer=args.socket_buffer,
                              chunk_kib=args.chunk_kib,
                              seconds=round(elapsed, 4), bytes=received,
                              mib_per_second=round(received / 1048576 / elapsed, 2))))


if __name__ == '__main__':
    main()
