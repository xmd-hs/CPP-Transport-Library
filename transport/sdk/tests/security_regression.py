#!/usr/bin/env python3
"""Real C ABI + TLS/adversarial-peer regression; requires Python and openssl.

Linux integration: --server <cloud_disk_server> additionally exercises stunnel.
No test certificates or private keys are installed into the user's trust store.
"""
import argparse
import contextlib
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import zlib


HOST = os.environ.get('CLDT_TEST_HOST', '127.0.0.1')


class Options(C.Structure):
    _fields_ = [('struct_size', C.c_size_t), ('connect_timeout_ms', C.c_uint32),
                ('io_timeout_ms', C.c_uint32), ('tls_enabled', C.c_int),
                ('ca_file', C.c_char_p), ('server_name', C.c_char_p),
                ('download_durability', C.c_int)]


DONE = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_uint64, C.c_char_p)
OPEN = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p, C.c_uint32, C.c_uint64,
                  C.POINTER(C.c_void_p), C.POINTER(C.c_uint64))
READ = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint64, C.c_void_p, C.c_size_t,
                  C.POINTER(C.c_size_t))
WRITE = READ
CLOSE = C.CFUNCTYPE(None, C.c_void_p)
SYNC = C.CFUNCTYPE(C.c_int, C.c_void_p)


class FileIO(C.Structure):
    _fields_ = [('struct_size', C.c_size_t), ('context', C.c_void_p),
                ('open', OPEN), ('read_at', READ), ('write_at', WRITE), ('close', CLOSE)]


def load(path):
    lib = C.CDLL(str(path))
    signatures = {
        'create': ([C.c_char_p, C.c_uint16, C.c_char_p, C.c_void_p, DONE, C.c_void_p], C.c_void_p),
        'destroy': ([C.c_void_p], None), 'cancel': ([C.c_void_p], C.c_int),
        'options_init': ([C.POINTER(Options)], None),
        'set_options': ([C.c_void_p, C.POINTER(Options)], C.c_int),
        'set_file_io': ([C.c_void_p, C.POINTER(FileIO)], C.c_int),
        'set_file_sync': ([C.c_void_p, SYNC], C.c_int),
        'stat': ([C.c_void_p, C.c_char_p], C.c_int),
        'upload': ([C.c_void_p, C.c_char_p, C.c_char_p, C.c_uint64], C.c_int),
        'download': ([C.c_void_p, C.c_char_p, C.c_char_p, C.c_uint64, C.c_uint64], C.c_int)}
    for name, (args, result) in signatures.items():
        fn = getattr(lib, 'cldt_client_' + name)
        fn.argtypes, fn.restype = args, result
        setattr(lib, name, fn)
    return lib


class Client:
    def __init__(self, lib, port, ca=None, name=b'localhost', token=b'test-token', host=HOST.encode()):
        self.lib, self.event, self.results = lib, threading.Event(), []
        def done(_, status, total, message):
            self.results.append((status, total, message.decode(errors='replace')))
            self.event.set()
        self.callback = DONE(done)
        self.handle = lib.create(host, port, token, None, self.callback, None)
        assert self.handle
        self.options = Options()
        lib.options_init(C.byref(self.options))
        self.options.connect_timeout_ms = 400
        self.options.io_timeout_ms = 200
        self.options.tls_enabled = ca is not None
        self.options.ca_file = os.fsencode(ca) if ca else None
        self.options.server_name = name
        assert self.configure() == 0

    def configure(self):
        return self.lib.set_options(self.handle, C.byref(self.options))

    def submit(self, mode='stat', local=None, offset=0, length=0, remote=b'data.bin'):
        self.event.clear()
        if mode == 'stat': return self.lib.stat(self.handle, remote)
        if mode == 'upload': return self.lib.upload(self.handle, os.fsencode(local), remote, offset)
        return self.lib.download(self.handle, remote, os.fsencode(local), offset, length)

    def wait(self, expected=0):
        assert self.event.wait(5), 'callback did not arrive'
        result = self.results[-1]
        assert result[0] == expected, (expected, result)
        return result

    def close(self):
        if self.handle:
            self.lib.destroy(self.handle)
            self.handle = None

    def __enter__(self): return self
    def __exit__(self, *args): self.close()


def frame(kind, body_len=0, offset=0, size=0, body=b'', corrupt=False):
    raw = struct.pack('<IBBHIQQQI', 0x434c4454, 2, kind, 0, 0,
                      body_len, offset, size, 0)
    crc = zlib.crc32(raw + body) ^ int(corrupt)
    return raw[:36] + struct.pack('<I', crc) + body


def exact(conn, length):
    data = b''
    while len(data) < length:
        block = conn.recv(length - len(data))
        if not block: raise EOFError()
        data += block
    return data


def request(conn):
    fields = struct.unpack('<IBBHIQQQI', exact(conn, 40))
    exact(conn, fields[4])
    return fields


class Peer:
    def __init__(self, action, context=None):
        self.sock = socket.socket()
        self.sock.bind((HOST, 0))
        self.sock.listen()
        self.sock.settimeout(.1)
        self.port = self.sock.getsockname()[1]
        self.stop = threading.Event()
        self.connections, self.errors = [], []
        def run():
            while not self.stop.is_set():
                try: conn, _ = self.sock.accept()
                except socket.timeout: continue
                except OSError: break
                self.connections.append(conn)
                try:
                    conn.settimeout(2)
                    if context:
                        conn = context.wrap_socket(conn, server_side=True)
                        self.connections.append(conn)
                    action(conn)
                except (OSError, EOFError): pass  # Negative TLS tests deliberately disconnect.
                except Exception as exc: self.errors.append(repr(exc))
                finally: conn.close()
        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()

    def __enter__(self): return self
    def __exit__(self, *args):
        self.stop.set()
        self.sock.close()
        for conn in self.connections:
            try: conn.shutdown(socket.SHUT_RDWR)
            except OSError: pass
            conn.close()
        self.thread.join(3)
        assert not self.thread.is_alive(), 'test peer did not exit'
        assert not self.errors, self.errors


def certs(work):
    def run(*args):
        subprocess.run(['openssl', *args], cwd=work, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
        '-subj', '/CN=CLDT test CA', '-keyout', 'ca.key', '-out', 'ca.pem')
    run('req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost',
        '-keyout', 'server.key', '-out', 'server.csr')
    (work / 'extensions').write_text(f'subjectAltName=DNS:localhost,IP:{HOST}\nextendedKeyUsage=serverAuth\n')
    run('x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key',
        '-CAcreateserial', '-days', '1', '-extfile', 'extensions', '-out', 'server.pem')
    (work / 'index').touch()
    (work / 'serial').write_text('1000\n')
    (work / 'ca.conf').write_text('[ca]\ndefault_ca=signer\n[signer]\n'
        'database=index\nserial=serial\nnew_certs_dir=.\ncertificate=ca.pem\n'
        'private_key=ca.key\ndefault_md=sha256\npolicy=policy\n[policy]\ncommonName=supplied\n')
    run('ca', '-batch', '-config', 'ca.conf', '-in', 'server.csr',
        '-startdate', '20000101000000Z', '-enddate', '20000102000000Z',
        '-extfile', 'extensions', '-out', 'expired.pem')
    run('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
        '-subj', '/CN=Untrusted', '-keyout', 'other.key', '-out', 'other.pem')


def context(work, expired=False):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(work / ('expired.pem' if expired else 'server.pem'), work / 'server.key')
    return ctx


def wait_port(port, process):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        assert process.poll() is None, 'server exited'
        try:
            with socket.create_connection((HOST, port), .1): return
        except OSError: time.sleep(.02)
    raise AssertionError('server not ready')


def port():
    with socket.socket() as sock:
        sock.bind((HOST, 0))
        return sock.getsockname()[1]


@contextlib.contextmanager
def services(server, work):
    plain, secure = port(), port()
    root = work / 'root'
    root.mkdir()
    config = work / 'stunnel.conf'
    config.write_text(f'foreground=yes\npid=\n[cldt]\naccept={HOST}:{secure}\n'
                      f'connect={HOST}:{plain}\ncert={work}/server.pem\n'
                      f'key={work}/server.key\nsslVersionMin=TLSv1.2\n')
    processes = []
    with (work / 'services.log').open('wb') as log:
        try:
            processes.append(subprocess.Popen([str(server), str(plain), str(root), '2', 'test-token'],
                             env=dict(os.environ, CLDT_BIND_IP=HOST), stdout=log, stderr=log))
            wait_port(plain, processes[-1])
            processes.append(subprocess.Popen(['stunnel', str(config)], stdout=log, stderr=log))
            wait_port(secure, processes[-1])
            yield plain, secure, root
        finally:
            for process in reversed(processes):
                process.terminate()
                try: process.wait(5)
                except subprocess.TimeoutExpired: process.kill(); process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--server', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--tls-unavailable', action='store_true',
                        help='Check a TLS-off binary rejects secure configuration')
    args = parser.parse_args()
    lib, passed = load(args.library.resolve()), []
    def check(name):
        passed.append(name)
        print('PASS', name, flush=True)
    if args.tls_unavailable:
        with Client(lib, port()) as client:
            client.options.tls_enabled = 1
            assert client.configure() == 1012
            client.options.tls_enabled = 0
            assert client.configure() == 0
        check('TLS-off fails closed, client remains usable')
        return
    with tempfile.TemporaryDirectory(prefix='cldt-security-') as tmp:
        work = Path(tmp)
        certs(work)
        ca, ctx = work / 'ca.pem', context(work)
        source, target = work / 'source', work / 'target'
        source.write_bytes(bytes(range(256)) * 8192)
        def good(conn): request(conn); conn.sendall(frame(7, size=123))
        for name, authority, hostname, expired, expected in [
            ('trusted TLS + DNS identity', ca, b'localhost', False, 0),
            ('trusted TLS + IP identity', ca, HOST.encode(), False, 0),
            ('wrong CA rejected', work / 'other.pem', b'localhost', False, 1011),
            ('wrong hostname rejected', ca, b'attacker.invalid', False, 1011),
            ('expired certificate rejected', ca, b'localhost', True, 1011)]:
            with Peer(good, context(work, expired)) as peer, Client(lib, peer.port, authority, hostname) as client:
                assert client.submit() == 0
                client.wait(expected)
            check(name)
        leaked = []
        def plaintext(conn): leaked.append(conn.recv(8192)); conn.sendall(b'not TLS\n')
        with Peer(plaintext) as peer, Client(lib, peer.port, ca) as client:
            assert client.submit() == 0
            client.wait(1011)
        assert leaked and all(b'test-token' not in block for block in leaked)
        check('no plaintext downgrade/token leakage')
        legacy = context(work)
        legacy.minimum_version = ssl.TLSVersion.TLSv1_1
        legacy.maximum_version = ssl.TLSVersion.TLSv1_1
        legacy.set_ciphers('DEFAULT:@SECLEVEL=0')
        with Peer(good, legacy) as peer, Client(lib, peer.port, ca) as client:
            assert client.submit() == 0
            client.wait(1011)
        check('TLS 1.1 rejected')
        def stalled(conn):
            while conn.recv(8192): pass
        for tls in (False, True):
            with Peer(stalled) as peer, Client(lib, peer.port, ca if tls else None) as client:
                start = time.monotonic()
                assert client.submit() == 0
                assert client.submit() == 1002
                assert client.configure() == 1002
                client.wait(1010)
                assert time.monotonic() - start < 2
                check('TLS handshake timeout/busy' if tls else 'read timeout/busy')
                assert client.submit() == 0
                time.sleep(.04)
                start = time.monotonic()
                assert lib.cancel(client.handle) == 0
                client.wait(1003)
                assert time.monotonic() - start < 1
                check('TLS handshake cancellation' if tls else 'read cancellation')
                assert client.submit() == 0
                client.wait(1010)
                check('reuse after cancellation TLS=' + str(tls))
        # Fill the socket send buffer: upload must time out/cancel even when
        # peer accepts but does not consume the file body.
        large = work / 'blocked-upload'
        with large.open('wb') as stream: stream.truncate(64 * 1024 * 1024)
        release = threading.Event()
        def no_read(conn):
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            release.wait(2)
        with Peer(no_read) as peer, Client(lib, peer.port) as client:
            assert client.submit('upload', large) == 0
            client.wait(1010)
            release.set()
        check('blocked upload inactivity timeout')
        release.clear()
        with Peer(no_read, ctx) as peer, Client(lib, peer.port, ca) as client:
            assert client.submit('upload', large) == 0
            time.sleep(.1)
            start = time.monotonic()
            lib.cancel(client.handle)
            client.wait(1003)
            assert time.monotonic() - start < 1
            release.set()
        check('blocked TLS upload cancellation')
        with Peer(stalled) as peer:
            client = Client(lib, peer.port, ca)
            assert client.submit() == 0
            time.sleep(.04)
            start = time.monotonic()
            client.close()
            assert time.monotonic() - start < 1
            assert len(client.results) == 1 and client.results[0][0] == 1003
        check('destroy joins worker with one cancellation callback')
        for mode in ('download', 'upload', 'stat'):
            def denied(conn):
                req = request(conn)
                if req[2] == 1: exact(conn, req[7] - req[6])
                conn.sendall(frame(5, body_len=6, offset=8, body=b'denied'))
            with Peer(denied, ctx) as peer, Client(lib, peer.port, ca) as client:
                assert client.submit(mode, source) == 0
                client.wait(2008)
            check('consistent remote errors ' + mode)
        invalid_frames = [frame(4, body_len=5, size=4), frame(4, size=2**63),
                          frame(4, offset=1, size=1), frame(4, corrupt=True),
                          frame(5, body_len=2**32), frame(5, offset=999)]
        for index, response in enumerate(invalid_frames):
            def malformed(conn): request(conn); conn.sendall(response)
            target.write_bytes(b'keep original')
            with Peer(malformed) as peer, Client(lib, peer.port) as client:
                assert client.submit('download', target) == 0
                client.wait(1006)
            assert target.read_bytes() == b'keep original'
            check('malformed response preserves destination ' + str(index))
        with Client(lib, port()) as client:
            assert client.submit(remote=b'../escape') == 1001
            assert client.submit('download', target, offset=2**63) == 1001
            assert client.submit('download', target, offset=2**63-1, length=1) == 1001
            assert client.submit('upload', '') == 1001
            client.options.io_timeout_ms = 0
            assert client.configure() == 1001
        check('synchronous argument rejection')
        # Custom I/O: short writes succeed; sync failure and invalid counts fail.
        for sync_result, oversized, expected in [(0, False, 0), (-1, False, 1007), (0, True, 1007)]:
            closed, synced, output = [], [], bytearray()
            @OPEN
            def opening(_, identifier, flags, size, handle, file_size):
                handle[0], file_size[0] = 1, 0
                return 0
            @READ
            def reading(*unused): return -1
            @WRITE
            def writing(_, offset, data, length, written):
                amount = min(2, length)
                output.extend(C.string_at(data, amount))
                written[0] = length + 1 if oversized else amount
                return 0
            @CLOSE
            def closing(_): closed.append(True)
            @SYNC
            def syncing(_): synced.append(True); return sync_result
            io = FileIO(C.sizeof(FileIO), None, opening, reading, writing, closing)
            def data_peer(conn): request(conn); conn.sendall(frame(4, body_len=5, size=5) + b'hello')
            with Peer(data_peer, ctx) as peer, Client(lib, peer.port, ca) as client:
                assert lib.set_file_io(client.handle, C.byref(io)) == 0
                assert client.submit('download', target) == 1012
                assert lib.set_file_sync(client.handle, syncing) == 0
                assert client.submit('download', target) == 0
                client.wait(expected)
            assert len(closed) == 1
            if not oversized: assert output == b'hello' and len(synced) == 1
            check(f'custom short writes/sync/counts {sync_result}/{oversized}')
        if args.server:
            with services(args.server.resolve(), work) as (plain, secure, root):
                for tls, endpoint in [(False, plain), (True, secure)]:
                    with Client(lib, endpoint, ca if tls else None) as client:
                        client.options.io_timeout_ms = 5000
                        assert client.configure() == 0
                        assert client.submit('upload', source) == 0
                        client.wait()
                        assert client.submit() == 0
                        assert client.wait()[1] == source.stat().st_size
                        assert client.submit('download', target) == 0
                        client.wait()
                        assert hashlib.sha256(source.read_bytes()).digest() == hashlib.sha256(target.read_bytes()).digest()
                        assert client.submit('download', target, offset=17, length=31) == 0
                        assert client.wait()[1] == 31
                        assert target.read_bytes() == source.read_bytes()
                        assert client.submit(remote=b'missing') == 0
                        client.wait(2004)
                    check('real server upload/stat/download/range SHA256 TLS=' + str(tls))
                large = work / 'large-source'
                with large.open('wb') as stream: stream.truncate(64 * 1024 * 1024)
                for tls, endpoint in [(False, plain), (True, secure)]:
                    with Client(lib, endpoint, ca if tls else None, token=b'incorrect') as client:
                        assert client.submit() == 0
                        client.wait(2008)
                        assert client.submit('upload', large) == 0
                        client.wait(2008)
                    check('real server authentication/early upload rejection TLS=' + str(tls))
    result = {'passed': len(passed), 'cases': passed,
              'library': str(args.library), 'real_server': bool(args.server)}
    if args.output: args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)


if __name__ == '__main__': main()
