#!/usr/bin/env bash
# 1 seed + 2 leech，以及停掉 seed 后用已完成节点继续做种的验收。
# 在已构建的源码树根目录旁执行；WSL 下若 127.0.0.1 UDP 异常，节点会自动探测宣告 IP。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/example/_run_demo.py"
