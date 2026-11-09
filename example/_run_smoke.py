#!/usr/bin/env python3
import os
import subprocess
import sys
import time

root = os.path.expanduser("~/chuanshuku_src")
os.chdir(root)
work = "/tmp/p2p_smoke_run"
subprocess.check_call(["rm", "-rf", work])
os.makedirs(work)
subprocess.check_call(
    ["dd", "if=/dev/urandom", "of=%s/data.bin" % work, "bs=1024", "count=256", "status=none"]
)
seed_log = open("%s/seed.log" % work, "w", buffering=1)
leech_log = open("%s/leech.log" % work, "w", buffering=1)
seed = subprocess.Popen(
    ["./example/p2p_seed", "%s/data.bin" % work, "%s/data.meta" % work, "17001", "18001"],
    stdout=seed_log,
    stderr=subprocess.STDOUT,
    cwd=root,
)
time.sleep(1.5)
leech = subprocess.Popen(
    [
        "./example/p2p_leech",
        "%s/data.meta" % work,
        "%s/out.bin" % work,
        "17002",
        "18002",
        "17001",
    ],
    stdout=leech_log,
    stderr=subprocess.STDOUT,
    cwd=root,
)
for i in range(60):
    time.sleep(0.5)
    if leech.poll() is not None:
        break
print("leech_rc", leech.poll(), "after", (i + 1) * 0.5, "s")
if leech.poll() is None:
    leech.terminate()
    leech.wait(timeout=3)
seed.terminate()
seed.wait(timeout=3)
seed_log.close()
leech_log.close()
print("===== SEED (tail) =====")
print("".join(open("%s/seed.log" % work).readlines()[-40:]))
print("===== LEECH (tail) =====")
print("".join(open("%s/leech.log" % work).readlines()[-60:]))
cmp = subprocess.call(["cmp", "-s", "%s/data.bin" % work, "%s/out.bin" % work])
print("CMP_OK" if cmp == 0 else "CMP_FAIL")
sys.exit(0 if cmp == 0 else 1)
