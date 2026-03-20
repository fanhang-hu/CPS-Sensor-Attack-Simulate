#!/usr/bin/env bash
set -euo pipefail

RUN_DIR="${1:-}"
if [[ -z "${RUN_DIR}" ]]; then
  echo "Usage: $0 <run_dir>" >&2
  exit 1
fi

ATTACKER_LOG="${RUN_DIR}/attacker.log"
CONTROLLER_LOG="${RUN_DIR}/controller.log"
PLANT_LOG="${RUN_DIR}/plant.log"

if [[ ! -f "${ATTACKER_LOG}" || ! -f "${CONTROLLER_LOG}" ]]; then
  echo "[check_replay_logs] missing attacker/controller logs in ${RUN_DIR}" >&2
  exit 1
fi

extract_metric() {
  local key="$1"
  local file="$2"
  local val
  val="$(awk -v target="${key}" '
    /summary/ {
      for (i = 1; i <= NF; ++i) {
        if ($i ~ ("^" target "=")) {
          split($i, kv, "=");
          print kv[2];
        }
      }
    }
  ' "${file}" | tail -n 1)"
  if [[ -z "${val}" ]]; then
    echo "0"
  else
    echo "${val}"
  fi
}

ROUNDS_DONE="$(extract_metric rounds_done "${ATTACKER_LOG}")"
REPLAY_PACKETS="$(extract_metric replay_packets "${ATTACKER_LOG}")"

SEQ_BACKTRACKS="$(awk '
  {
    if (match($0, /seq=[0-9]+/)) {
      s = substr($0, RSTART + 4, RLENGTH - 4) + 0;
      if (seen && s < prev) {
        backtracks++;
      }
      prev = s;
      seen = 1;
    }
  }
  END { print backtracks + 0; }
' "${CONTROLLER_LOG}")"

if [[ -f "${PLANT_LOG}" ]]; then
  read -r MIN_SPEED MAX_SPEED < <(awk '
    {
      if (match($0, /wheel_speed=[-0-9.]+/)) {
        v = substr($0, RSTART + 12, RLENGTH - 12) + 0;
        if (!seen || v < minv) minv = v;
        if (!seen || v > maxv) maxv = v;
        seen = 1;
      }
    }
    END {
      if (!seen) {
        print "nan nan";
      } else {
        printf "%.6f %.6f\n", minv, maxv;
      }
    }
  ' "${PLANT_LOG}")
else
  MIN_SPEED="nan"
  MAX_SPEED="nan"
fi

echo "[check_replay_logs] rounds_done=${ROUNDS_DONE} replay_packets=${REPLAY_PACKETS} seq_backtracks=${SEQ_BACKTRACKS}"
echo "[check_replay_logs] wheel_speed_min=${MIN_SPEED} wheel_speed_max=${MAX_SPEED}"

if [[ "${REPLAY_PACKETS}" -gt 0 && "${SEQ_BACKTRACKS}" -gt 0 ]]; then
  echo "[check_replay_logs] verdict=PASS (replay evidence found)"
  exit 0
fi

echo "[check_replay_logs] verdict=FAIL (insufficient replay evidence)" >&2
exit 2
