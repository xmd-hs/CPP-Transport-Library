#!/usr/bin/env python3
import os
import socket
import subprocess
import time

root = os.path.expanduser("~/chuanshuku_src")
os.chdir(root)
work = "/tmp/p2p_udp_probe"
subprocess.check_call(["rm", "-rf", work])
os.makedirs(work)
subprocess.check_call(
    ["dd", "if=/dev/urandom", "of=%s/data.bin" % work, "bs=1024", "count=64", "status=none"]
)
log = open("%s/seed.log" % work, "w", buffering=1)
seed = subprocess.Popen(
    ["./example/p2p_seed", "%s/data.bin" % work, "%s/data.meta" % work, "17001", "18001"],
    stdout=log,
    stderr=subprocess.STDOUT,
)
time.sleep(1.5)
print("ss:")
subprocess.call(["bash", "-lc", "ss -ulnp | grep 17001 || netstat -ulnp 2>/dev/null | grep 17001 || true"])
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
n = sock.sendto(b"HELLOUDP", ("127.0.0.1", 17001))
print("sent", n, "bytes")
time.sleep(1)
sock.close()
seed.terminate()
seed.wait(timeout=2)
log.close()
print("===== SEED =====")
print(open("%s/seed.log" % work).read())
