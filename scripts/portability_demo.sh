#!/usr/bin/env bash
# The same run deployed two ways, with the same binaries:
#   A: 2 targets x 1 application x 4 workers, applications talking over TCP
#   B: 1 target  x 2 applications x 4 workers, applications talking over shared memory
# Both must agree with the Python reference, and their fields must be equal
# byte for byte.
#
#   portability_demo.sh [--bin DIR] [--out DIR] [--width W] [--height H] [--steps N]
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT_DIR}/build/release/apps"
OUT="$(mktemp -d)"
EXTRA=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --bin) BIN="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --width|--height|--steps|--every|--seed) EXTRA+=("$1" "$2"); shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
JOB=(--sim 6 --analysis 2 --bin "${BIN}" --compare "${EXTRA[@]}")

echo "== A: 2 targets x 1 application x 4 workers over TCP"
"${ROOT_DIR}/scripts/run_local.sh" --layout 0:0:4,1:0:4 --link tcp --out "${OUT}/a" "${JOB[@]}"
echo
echo "== B: 1 target x 2 applications x 4 workers over shared memory"
"${ROOT_DIR}/scripts/run_local.sh" --layout 0:0:4,0:1:4 --link shm --out "${OUT}/b" "${JOB[@]}"
echo
for f in U.bin V.bin; do
  cmp "${OUT}/a/${f}" "${OUT}/b/${f}"
done
echo "A and B: identical fields (U.bin, V.bin byte for byte)"
