#!/usr/bin/env bash
# Runs the Gray-Scott demonstrator on this machine: the coordinator and one
# process per entry of the layout. Several targets are simulated by processes
# of one machine; --link selects how the applications talk.
#
#   run_local.sh --layout L --sim S --analysis A --out DIR
#                [--link tcp|shm] [--width W] [--height H] [--steps N]
#                [--every K] [--seed S] [--bin DIR] [--compare]
#
# Example (2 targets x 1 application x 4 workers, over TCP):
#   run_local.sh --layout 0:0:4,1:0:4 --sim 6 --analysis 2 --out /tmp/gs --compare
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LINK=tcp WIDTH=256 HEIGHT=256 STEPS=1000 EVERY=100 SEED=1
BIN="${ROOT_DIR}/build/release/apps"
COMPARE=0 LAYOUT="" SIM="" ANALYSIS="" OUT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --layout) LAYOUT="$2"; shift 2 ;;
    --link) LINK="$2"; shift 2 ;;
    --sim) SIM="$2"; shift 2 ;;
    --analysis) ANALYSIS="$2"; shift 2 ;;
    --width) WIDTH="$2"; shift 2 ;;
    --height) HEIGHT="$2"; shift 2 ;;
    --steps) STEPS="$2"; shift 2 ;;
    --every) EVERY="$2"; shift 2 ;;
    --seed) SEED="$2"; shift 2 ;;
    --bin) BIN="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --compare) COMPARE=1; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [[ -z "${LAYOUT}" || -z "${SIM}" || -z "${ANALYSIS}" || -z "${OUT}" ]]; then
  echo "usage: $0 --layout L --sim S --analysis A --out DIR [--link tcp|shm] [...]" >&2
  exit 2
fi
mkdir -p "${OUT}"

# One configuration for every process. Targets are 127.0.0.1 with their own
# port ranges; port = base + app * 32 + inst (pushseg's tcp backend).
PORT_BASE=$((10000 + ($$ % 100) * 200))
mapfile -t NODES < <( (echo 0; tr ',' '\n' <<<"${LAYOUT}" | cut -d: -f1) | sort -un)
{
  echo "[apps]"
  echo "coordinator = 1"
  echo "worker = 2"
  for n in "${NODES[@]}"; do
    echo "[node.${n}]"
    echo "tcp = 127.0.0.1:$((PORT_BASE + n * 100))"
    echo "shm ="
  done
  echo "[links]"
  echo "default = ${LINK}"
  echo "[backend.shm]"
  echo "name_prefix = /gridlane-run-$$"
} >"${OUT}/system.conf"

PIDS=()
cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
}
trap cleanup EXIT

IFS=',' read -ra PROCS <<<"${LAYOUT}"
for proc in "${PROCS[@]}"; do
  IFS=':' read -r node inst _ <<<"${proc}"
  "${BIN}/gridlane_worker" --system "${OUT}/system.conf" --layout "${LAYOUT}" \
    --node "${node}" --inst "${inst}" --out "${OUT}" >"${OUT}/worker-${node}-${inst}.log" 2>&1 &
  PIDS+=($!)
done

"${BIN}/gridlane_coordinator" --system "${OUT}/system.conf" --layout "${LAYOUT}" \
  --sim "${SIM}" --analysis "${ANALYSIS}" --width "${WIDTH}" --height "${HEIGHT}" \
  --steps "${STEPS}" --every "${EVERY}" --seed "${SEED}" --report "${OUT}/report.json"

status=0
for p in "${PIDS[@]}"; do
  wait "$p" || status=1
done
PIDS=()
if [[ ${status} -ne 0 ]]; then
  echo "a worker failed; see ${OUT}/worker-*.log" >&2
  exit 1
fi
if [[ ${COMPARE} -eq 1 ]]; then
  python3 "${ROOT_DIR}/python/compare.py" --run "${OUT}"
fi
