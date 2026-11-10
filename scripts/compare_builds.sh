#!/usr/bin/env bash
# Compare two already-built revisions, both retaining the xmdMemoryPool pool.
set -euo pipefail
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: bash scripts/compare_builds.sh /absolute/before/example /absolute/after/example [host]" >&2
    exit 2
fi
before=$(realpath "$1")
after=$(realpath "$2")
host=${3:-127.0.0.1}
cd "$(dirname "$0")/.."
for trial in 1 2 3 4 5; do
    order='before after'
    if [ $((trial % 2)) = 0 ]; then order='after before'; fi
    for version in $order; do
        bindir="$before"
        if [ "$version" = after ]; then bindir="$after"; fi
        for clients in 1 4; do
            size=128
            if [ "$clients" = 1 ]; then size=512; fi
            echo "version=$version trial=$trial clients=$clients"
            python3 scripts/bench_transfer.py --bin-dir "$bindir" --host "$host" --size-mib "$size" \
                --clients "$clients" --work-dir /dev/shm
        done
    done
done
