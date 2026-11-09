#!/usr/bin/env python3
"""32MiB seed->leech timing smoke."""
import os
import subprocess
import sys
import time

root = os.path.expanduser("~/chuanshuku_src")
os.chdir(root)
work = "/tmp/p2p_perf_run"
subprocess.check_call(["rm", "-rf", work])
os.makedirs(work)
subprocess.check_call(
    ["dd", "if=/dev/urandom", "of=%s/data.bin" % work, "bs=1M", "count=64", "status=none"]
)
seed = subprocess.Popen(
    ["./example/p2p_seed", "%s/data.bin" % work, "%s/data.meta" % work, "17101", "18101"],
    stdout=open("%s/seed.log" % work, "w", buffering=1),
    stderr=subprocess.STDOUT,
)
time.sleep(2.0)
t0 = time.time()
leech = subprocess.run(
    [
        "./example/p2p_leech",
        "%s/data.meta" % work,
        "%s/out.bin" % work,
        "17102",
        "18102",
        "17101",
    ],
    stdout=open("%s/leech.log" % work, "w", buffering=1),
    stderr=subprocess.STDOUT,
)
elapsed = time.time() - t0
seed.terminate()
try:
    seed.wait(timeout=2)
except Exception:
    seed.kill()
ok = leech.returncode == 0 and subprocess.call(
    ["cmp", "-s", "%s/data.bin" % work, "%s/out.bin" % work]
) == 0
mbps = (64.0 / elapsed) if elapsed > 0 else 0
print("ok=%s size=64MiB elapsed=%.3fs throughput=%.1f MiB/s" % (ok, elapsed, mbps))
if not ok:
    print("".join(open("%s/leech.log" % work).readlines()[-40:]))
sys.exit(0 if ok else 1)
