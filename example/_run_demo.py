#!/usr/bin/env python3
"""1 seed + 2 leech, then former-leech reseeds for leech3."""
import os
import subprocess
import sys
import time

root = os.path.expanduser("~/chuanshuku_src")
os.chdir(root)
work = "/tmp/p2p_demo_run"
subprocess.check_call(["rm", "-rf", work])
os.makedirs(work)
subprocess.check_call(
    ["dd", "if=/dev/urandom", "of=%s/data.bin" % work, "bs=1024", "count=512", "status=none"]
)


def run_logged(cmd, log_path):
    return subprocess.Popen(
        cmd, stdout=open(log_path, "w", buffering=1), stderr=subprocess.STDOUT, cwd=root
    )


def wait_ok(proc, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline and proc.poll() is None:
        time.sleep(0.2)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except Exception:
            proc.kill()
        return False
    return proc.returncode == 0


def dump(name):
    path = "%s/%s" % (work, name)
    print("===== %s =====" % name)
    if os.path.exists(path):
        print("".join(open(path).readlines()[-50:]))


seed = run_logged(
    ["./example/p2p_seed", "%s/data.bin" % work, "%s/data.meta" % work, "17001", "18001"],
    "%s/seed.log" % work,
)
time.sleep(1.5)

l1 = run_logged(
    ["./example/p2p_leech", "%s/data.meta" % work, "%s/out1.bin" % work, "17002", "18002", "17001"],
    "%s/leech1.log" % work,
)
time.sleep(0.4)
l2 = run_logged(
    ["./example/p2p_leech", "%s/data.meta" % work, "%s/out2.bin" % work, "17003", "18003", "17001"],
    "%s/leech2.log" % work,
)

# wait both in parallel
deadline = time.time() + 90
while time.time() < deadline and (l1.poll() is None or l2.poll() is None):
    time.sleep(0.2)

ok1 = l1.poll() == 0
ok2 = l2.poll() == 0
if l1.poll() is None:
    l1.terminate()
if l2.poll() is None:
    l2.terminate()

if not (ok1 and ok2):
    seed.terminate()
    print("FAIL phase1 leech1=%s(%s) leech2=%s(%s)" % (ok1, l1.poll(), ok2, l2.poll()))
    dump("seed.log")
    dump("leech1.log")
    dump("leech2.log")
    sys.exit(1)
subprocess.check_call(["cmp", "-s", "%s/data.bin" % work, "%s/out1.bin" % work])
subprocess.check_call(["cmp", "-s", "%s/data.bin" % work, "%s/out2.bin" % work])
print("PASS: seed + 2 leech")

seed.terminate()
try:
    seed.wait(timeout=2)
except Exception:
    seed.kill()
time.sleep(0.5)

seed2 = run_logged(
    ["./example/p2p_seed", "%s/out1.bin" % work, "%s/data.meta" % work, "17011", "18011"],
    "%s/seed2.log" % work,
)
time.sleep(1.5)
l3 = run_logged(
    ["./example/p2p_leech", "%s/data.meta" % work, "%s/out3.bin" % work, "17004", "18004", "17011"],
    "%s/leech3.log" % work,
)
ok3 = wait_ok(l3, 90)
seed2.terminate()
try:
    seed2.wait(timeout=2)
except Exception:
    seed2.kill()
if not ok3:
    print("FAIL leech3 rc=%s" % l3.poll())
    dump("seed2.log")
    dump("leech3.log")
    sys.exit(1)
subprocess.check_call(["cmp", "-s", "%s/data.bin" % work, "%s/out3.bin" % work])
print("PASS: leech3 from former-leech seed")
print("ALL PASS")
