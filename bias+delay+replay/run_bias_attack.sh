#!/usr/bin/env bash
set -euo pipefail

META_FILE_PATH="${1:-/tmp/cps_controller_meta.txt}"
BIAS="${2:-4.0}"
INTERVAL_MS="${3:-100}"
ROUNDS="${4:-80}"
SYMBOL="${5:-g_latest_measurement}"
PROC_NAME="${6:-controller}"
TARGET_SPEC="${7:-auto}"

if [[ ! -f "${META_FILE_PATH}" ]]; then
  echo "[run_bias_attack] metadata file not found: ${META_FILE_PATH}" >&2
  exit 1
fi

if [[ "${TARGET_SPEC}" == "auto" ]]; then
  PID="$(awk -F= '/^pid=/{print $2}' "${META_FILE_PATH}" || true)"
  if [[ -n "${PID}" && "${PID}" =~ ^[0-9]+$ ]]; then
    TARGET_SPEC="${PID}"
  else
    TARGET_SPEC="auto"
  fi
fi

echo "[run_bias_attack] target=${TARGET_SPEC} bias=${BIAS} interval_ms=${INTERVAL_MS} rounds=${ROUNDS} symbol=${SYMBOL} proc_name=${PROC_NAME}"
exec ./bin/attacker_bias "${TARGET_SPEC}" "${BIAS}" "${INTERVAL_MS}" "${ROUNDS}" "${SYMBOL}" "${PROC_NAME}"
