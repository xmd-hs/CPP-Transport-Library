#!/usr/bin/env bash
# Build in a clean Linux directory; preserve the checkout and its existing artifacts.
set -euo pipefail
cd "$(dirname "$0")/.."
taskdir=$(mktemp -d "$HOME/muduo-validate-XXXXXX")
printf 'Validation workspace: %s\n' "$taskdir"
cp -r CMakeLists.txt src include transport third_party example "$taskdir/"
cmake -S "$taskdir" -B "$taskdir/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$taskdir/build" -j 4
ctest --test-dir "$taskdir/build" --output-on-failure
host=${1:-127.0.0.1}
python3 scripts/test_transfer.py --bin-dir "$taskdir/example" --host "$host"
python3 scripts/bench_transfer.py --bin-dir "$taskdir/example" --host "$host" --size-mib 128 --clients 1
python3 scripts/bench_transfer.py --bin-dir "$taskdir/example" --host "$host" --size-mib 128 --clients 4
