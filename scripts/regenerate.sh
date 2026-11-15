#!/usr/bin/env bash
# Regenerates generated/ from model/project.yaml with the pinned wireforge.
# --check: regenerate into a temporary directory and fail if it differs.
set -euo pipefail

WIREFORGE_VERSION="0.5.1"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

if ! command -v wireforge >/dev/null 2>&1; then
  echo "error: wireforge not found; pip install wireforge==${WIREFORGE_VERSION}" >&2
  exit 1
fi
if ! wireforge version | grep -q "wireforge ${WIREFORGE_VERSION} "; then
  echo "error: need wireforge ${WIREFORGE_VERSION}, found: $(wireforge version)" >&2
  exit 1
fi

wireforge validate model
wireforge fmt model --check

if [[ "${1:-}" == "--check" ]]; then
  out="$(mktemp -d)"
  trap 'rm -rf "${out}"' EXIT
  wireforge generate model -o "${out}" >/dev/null
  if ! diff -r --exclude=.clang-format "${out}" generated; then
    echo "error: generated/ is out of date; run scripts/regenerate.sh" >&2
    exit 1
  fi
  echo "generated/ is up to date"
else
  # Keep generated/.clang-format: it exempts the generated code from formatting.
  find generated -mindepth 1 -maxdepth 1 ! -name .clang-format -exec rm -rf {} +
  wireforge generate model -o generated
fi
