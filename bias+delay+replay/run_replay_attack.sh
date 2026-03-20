#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

META_FILE_PATH="${1:-/tmp/cps_controller_meta.txt}"
SENSOR_LOG_PATH="${2:-}"
HOLD_MS="${3:-220}"
INTERVAL_MS="${4:-320}"
ROUNDS="${5:-50}"
TARGET_PROC_NAME="${6:-sensor}"
SEND_INTERVAL_MS="${7:-40}"
CAPTURE_MIN="${8:-20}"
REPLAY_WINDOW="${9:-12}"
JITTER_MS="${10:-20}"
TARGET_SPEC="${11:-auto}"

if [[ ! -f "${META_FILE_PATH}" ]]; then
  echo "[run_replay_attack] metadata file not found: ${META_FILE_PATH}" >&2
  exit 1
fi

if [[ -z "${SENSOR_LOG_PATH}" ]]; then
  echo "[run_replay_attack] sensor log path is empty" >&2
  exit 1
fi

if [[ ! -f "${SENSOR_LOG_PATH}" ]]; then
  echo "[run_replay_attack] sensor log not found: ${SENSOR_LOG_PATH}" >&2
  exit 1
fi

if [[ "${TARGET_SPEC}" == "auto" && "${TARGET_PROC_NAME}" == "controller" ]]; then
  PID="$(awk -F= '/^pid=/{print $2}' "${META_FILE_PATH}" || true)"
  if [[ -n "${PID}" && "${PID}" =~ ^[0-9]+$ ]]; then
    TARGET_SPEC="${PID}"
  fi
fi

echo "[run_replay_attack] target=${TARGET_SPEC} target_proc=${TARGET_PROC_NAME} hold_ms=${HOLD_MS} interval_ms=${INTERVAL_MS} rounds=${ROUNDS} send_interval_ms=${SEND_INTERVAL_MS} capture_min=${CAPTURE_MIN} replay_window=${REPLAY_WINDOW} jitter_ms=${JITTER_MS} sensor_log=${SENSOR_LOG_PATH}"
exec "${ROOT_DIR}/bin/attacker_replay" \
  "${TARGET_SPEC}" \
  "${SENSOR_LOG_PATH}" \
  "${HOLD_MS}" \
  "${INTERVAL_MS}" \
  "${ROUNDS}" \
  "${TARGET_PROC_NAME}" \
  "${SEND_INTERVAL_MS}" \
  "${CAPTURE_MIN}" \
  "${REPLAY_WINDOW}" \
  "${JITTER_MS}"
