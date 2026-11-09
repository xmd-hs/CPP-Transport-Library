#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d /tmp/p2p-smoke-XXXXXX)
echo "P2P logs: $work"
seed_pid=
cleanup() {
    if [ -n "$seed_pid" ]; then
        kill "$seed_pid" 2>/dev/null || true
        wait "$seed_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
dd if=/dev/urandom of="$work/data.bin" bs=1024 count=256 status=none
./example/p2p_seed "$work/data.bin" "$work/data.meta" 17001 18001 >"$work/seed.log" 2>&1 &
seed_pid=$!
sleep 1
if timeout 25 ./example/p2p_leech "$work/data.meta" "$work/out.bin" 17002 18002 17001 >"$work/leech.log" 2>&1 && cmp -s "$work/data.bin" "$work/out.bin"; then
    echo CMP_OK
else
    tail -50 "$work/seed.log" || true
    tail -80 "$work/leech.log" || true
    echo CMP_FAIL
    exit 1
fi
