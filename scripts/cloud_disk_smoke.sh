#!/usr/bin/env bash
set -euo pipefail
SRC="$HOME/chuanshuku_src"
cd "$SRC"
rm -rf "$HOME/cloud_root" "$HOME/cloud_test"
mkdir -p "$HOME/cloud_root" "$HOME/cloud_test"
dd if=/dev/urandom of="$HOME/cloud_test/small.bin" bs=1M count=2 status=none
dd if=/dev/urandom of="$HOME/cloud_test/big.bin" bs=1M count=32 status=none

./example/cloud_disk_server 19000 "$HOME/cloud_root" 2 >"$HOME/cloud_server.log" 2>&1 &
SPID=$!
cleanup() { kill "$SPID" 2>/dev/null || true; wait "$SPID" 2>/dev/null || true; }
trap cleanup EXIT
sleep 0.4

echo "=== upload small ==="
./example/cloud_disk_client upload 127.0.0.1 19000 "$HOME/cloud_test/small.bin" docs/small.bin
echo "=== download small ==="
./example/cloud_disk_client download 127.0.0.1 19000 docs/small.bin "$HOME/cloud_test/small.out"
cmp "$HOME/cloud_test/small.bin" "$HOME/cloud_test/small.out"
echo "small OK"

echo "=== upload big ==="
./example/cloud_disk_client upload 127.0.0.1 19000 "$HOME/cloud_test/big.bin" docs/big.bin
echo "=== download big ==="
./example/cloud_disk_client download 127.0.0.1 19000 docs/big.bin "$HOME/cloud_test/big.out"
cmp "$HOME/cloud_test/big.bin" "$HOME/cloud_test/big.out"
echo "big OK"

echo "=== resume upload ==="
HALF=$((16 * 1024 * 1024))
truncate -s "$HALF" "$HOME/cloud_root/docs/big.bin"
./example/cloud_disk_client upload 127.0.0.1 19000 "$HOME/cloud_test/big.bin" docs/big.bin "$HALF"
./example/cloud_disk_client download 127.0.0.1 19000 docs/big.bin "$HOME/cloud_test/big.resume"
cmp "$HOME/cloud_test/big.bin" "$HOME/cloud_test/big.resume"
echo "resume OK"
echo ALL_PASS
