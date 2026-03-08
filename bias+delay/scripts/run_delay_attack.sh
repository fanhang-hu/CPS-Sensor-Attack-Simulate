#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

META_FILE_PATH="${1:-/tmp/cps_controller_meta.txt}"
HOLD_MS="${2:-180}"
INTERVAL_MS="${3:-250}"
ROUNDS="${4:-80}"
TARGET_PROC_NAME="${5:-sensor}"
JITTER_MS="${6:-30}"
TARGET_SPEC="${7:-auto}"

if [[ ! -f "${META_FILE_PATH}" ]]; then
  echo "[run_delay_attack] metadata file not found: ${META_FILE_PATH}" >&2
  exit 1
fi

if [[ "${TARGET_SPEC}" == "auto" && "${TARGET_PROC_NAME}" == "controller" ]]; then
  PID="$(awk -F= '/^pid=/{print $2}' "${META_FILE_PATH}" || true)"
  if [[ -n "${PID}" && "${PID}" =~ ^[0-9]+$ ]]; then
    TARGET_SPEC="${PID}"
  fi
fi

echo "[run_delay_attack] target=${TARGET_SPEC} target_proc=${TARGET_PROC_NAME} hold_ms=${HOLD_MS} interval_ms=${INTERVAL_MS} rounds=${ROUNDS} jitter_ms=${JITTER_MS}"
exec "${ROOT_DIR}/bin/attacker_delay" "${TARGET_SPEC}" "${HOLD_MS}" "${INTERVAL_MS}" "${ROUNDS}" "${TARGET_PROC_NAME}" "${JITTER_MS}"
