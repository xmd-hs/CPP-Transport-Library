#!/usr/bin/env bash
# Build independent copies: allocator-dependent public types must never be mixed.
set -euo pipefail
cd "$(dirname "$0")/.."
taskroot=$(mktemp -d "$HOME/muduo-allocator-ab-XXXXXX")
printf 'A/B workspaces and logs: %s\n' "$taskroot"
for allocator in pool system; do
    mkdir "$taskroot/$allocator"
    cp -r CMakeLists.txt src include transport third_party example "$taskroot/$allocator/"
    enabled=ON
    if [ "$allocator" = system ]; then enabled=OFF; fi
    cmake -S "$taskroot/$allocator" -B "$taskroot/$allocator/build" \
        -DCMAKE_BUILD_TYPE=Release -DMUDUO_USE_MEMORY_POOL="$enabled" \
        > "$taskroot/$allocator/configure.log" 2>&1
    cmake --build "$taskroot/$allocator/build" -j 4 \
        > "$taskroot/$allocator/build.log" 2>&1
    ctest --test-dir "$taskroot/$allocator/build" --output-on-failure
done
"$taskroot/pool/example/bench_allocator" | tee "$taskroot/allocator.log"
# Alternate execution order to limit warmup/order bias. All rows validate SHA256.
for trial in 1 2 3; do
    order='pool system'
    if [ "$trial" = 2 ]; then order='system pool'; fi
    for allocator in $order; do
        for clients in 1 4; do
            printf 'allocator=%s trial=%s clients=%s\n' "$allocator" "$trial" "$clients"
            python3 scripts/bench_transfer.py --bin-dir "$taskroot/$allocator/example" \
                --size-mib 32 --clients "$clients" --work-dir /dev/shm \
                | tee -a "$taskroot/$allocator/transfer.jsonl"
        done
    done
done
