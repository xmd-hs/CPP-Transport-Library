#!/usr/bin/env python3
"""Alternate SDK executables with identical transfer semantics and retain raw data."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--bin-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--host', required=True)
    parser.add_argument('--work-dir', type=Path, default=Path('/dev/shm'))
    parser.add_argument('--clients', type=int, default=4)
    parser.add_argument('--mode', choices=('both', 'upload', 'download'), default='both')
    parser.add_argument('--size-mib', type=int, default=256)
    parser.add_argument('--rounds', type=int, default=5)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error('rounds must be positive')
    records = []
    benchmark = Path(__file__).with_name('bench_transfer.py')
    # One excluded warm-up per executable; reverse measured order every round.
    for round_index in range(-1, args.rounds):
        order = ('before', 'after') if round_index % 2 == 0 else ('after', 'before')
        for version in order:
            command = [sys.executable, str(benchmark), '--bin-dir', str(args.bin_dir),
                       '--sdk-sample', str(getattr(args, version)), '--client-kind', 'sdk',
                       '--host', args.host, '--work-dir', str(args.work_dir),
                       '--clients', str(args.clients), '--size-mib', str(args.size_mib),
                       '--mode', args.mode,
                       '--server-disk-workers', '4']
            result = subprocess.run(command, check=True, capture_output=True, text=True,
                                    timeout=300)
            for line in result.stdout.splitlines():
                record = json.loads(line)
                record.update(version=version, round=round_index, warmup=round_index < 0)
                records.append(record)
                print(json.dumps(record), flush=True)
            args.output.write_text(json.dumps(records, indent=2) + '\n')
    modes = ('upload', 'download') if args.mode == 'both' else (args.mode,)
    for mode in modes:
        for version in ('before', 'after'):
            samples = [r for r in records if r['mode'] == mode and
                       r['version'] == version and not r['warmup']]
            print(version, mode, 'median MiB/s:',
                  statistics.median(r['mib_per_second'] for r in samples),
                  'median CPU seconds:', statistics.median(r['cpu_seconds'] for r in samples))


if __name__ == '__main__':
    main()
