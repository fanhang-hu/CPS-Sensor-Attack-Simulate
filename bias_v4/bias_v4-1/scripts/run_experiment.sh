#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

MODE="baseline"
DURATION_SEC=30
OUT_BASE_DIR="${ROOT_DIR}/logs"
META_FILE=""

SETPOINT="20"
KP="0.8"
CONTROLLER_WINDOW_MS="80"

SENSOR_BASE="20"
SENSOR_AMP="1.5"
SENSOR_INTERVAL_MS="100"

WARMUP_SEC=2
ATTACK_BIAS="4.0"
ATTACK_INTERVAL_MS="100"
ATTACK_ROUNDS="120"
ATTACK_SYMBOL="g_latest_measurement"
ATTACK_PROC_NAME="controller"
PTRACE_COMPAT="auto"

usage() {
  cat <<'USAGE_EOF'
Usage:
  ./scripts/run_experiment.sh [options]

Options:
  --mode baseline|bias        Run baseline (no attack) or bias attack scenario
  --duration-sec N            Total run time in seconds (default: 30)
  --out-dir DIR               Output base directory (default: ./logs)
  --meta-file PATH            Controller metadata file (default: <run_dir>/cps_controller_meta.txt)

  --setpoint V                Controller setpoint (default: 20)
  --kp V                      Controller Kp (default: 0.8)
  --controller-window-ms N    Controller attack window in ms (default: 80)

  --sensor-base V             Sensor base value (default: 20)
  --sensor-amp V              Sensor amplitude (default: 1.5)
  --sensor-interval-ms N      Sensor send interval in ms (default: 100)

  --warmup-sec N              Seconds before launching attacker in bias mode (default: 2)
  --attack-bias V             Added bias each attack round (default: 4.0)
  --attack-interval-ms N      Attack interval in ms (default: 100)
  --attack-rounds N           Attack rounds (default: 120)
  --attack-symbol NAME        Target symbol name (default: g_latest_measurement)
  --attack-proc-name NAME     Target process name for auto mode (default: controller)
  --ptrace-compat MODE        auto|0|1. 1 enables PR_SET_PTRACER_ANY in controller
  -h, --help                  Show this help
USAGE_EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      MODE="${2:-}"
      shift 2
      ;;
    --duration-sec)
      DURATION_SEC="${2:-}"
      shift 2
      ;;
    --out-dir)
      OUT_BASE_DIR="${2:-}"
      shift 2
      ;;
    --meta-file)
      META_FILE="${2:-}"
      shift 2
      ;;
    --setpoint)
      SETPOINT="${2:-}"
      shift 2
      ;;
    --kp)
      KP="${2:-}"
      shift 2
      ;;
    --controller-window-ms)
      CONTROLLER_WINDOW_MS="${2:-}"
      shift 2
      ;;
    --sensor-base)
      SENSOR_BASE="${2:-}"
      shift 2
      ;;
    --sensor-amp)
      SENSOR_AMP="${2:-}"
      shift 2
      ;;
    --sensor-interval-ms)
      SENSOR_INTERVAL_MS="${2:-}"
      shift 2
      ;;
    --warmup-sec)
      WARMUP_SEC="${2:-}"
      shift 2
      ;;
    --attack-bias)
      ATTACK_BIAS="${2:-}"
      shift 2
      ;;
    --attack-interval-ms)
      ATTACK_INTERVAL_MS="${2:-}"
      shift 2
      ;;
    --attack-rounds)
      ATTACK_ROUNDS="${2:-}"
      shift 2
      ;;
    --attack-symbol)
      ATTACK_SYMBOL="${2:-}"
      shift 2
      ;;
    --attack-proc-name)
      ATTACK_PROC_NAME="${2:-}"
      shift 2
      ;;
    --ptrace-compat)
      PTRACE_COMPAT="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[run_experiment] Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ "${MODE}" == "basline" ]]; then
  echo "[run_experiment] mode=basline detected, auto-correct to baseline"
  MODE="baseline"
fi

if [[ "${MODE}" != "baseline" && "${MODE}" != "bias" ]]; then
  echo "[run_experiment] --mode must be baseline or bias" >&2
  exit 1
fi

if [[ "${PTRACE_COMPAT}" != "auto" && "${PTRACE_COMPAT}" != "0" && "${PTRACE_COMPAT}" != "1" ]]; then
  echo "[run_experiment] --ptrace-compat must be auto, 0, or 1" >&2
  exit 1
fi

need_rebuild=0
for b in controller sensor attacker_bias; do
  if [[ ! -x "${ROOT_DIR}/bin/${b}" ]]; then
    need_rebuild=1
  fi
done

if [[ "$(uname -s)" == "Linux" ]] && command -v file >/dev/null 2>&1; then
  for b in controller sensor attacker_bias; do
    if [[ -x "${ROOT_DIR}/bin/${b}" ]]; then
      if ! file -b "${ROOT_DIR}/bin/${b}" | grep -q "ELF"; then
        echo "[run_experiment] detected non-ELF binary: bin/${b}"
        need_rebuild=1
      fi
    fi
  done
fi

if [[ "${need_rebuild}" -eq 1 ]]; then
  echo "[run_experiment] rebuilding binaries with make clean && make ..."
  (cd "${ROOT_DIR}" && make clean && make)
fi

effective_ptrace_compat="${PTRACE_COMPAT}"
if [[ "${MODE}" == "bias" && "${PTRACE_COMPAT}" == "auto" ]]; then
  has_ptrace_cap=0
  if command -v getcap >/dev/null 2>&1; then
    if getcap "${ROOT_DIR}/bin/attacker_bias" 2>/dev/null | grep -q "cap_sys_ptrace"; then
      has_ptrace_cap=1
    fi
  fi

  ptrace_scope="1"
  if [[ -r /proc/sys/kernel/yama/ptrace_scope ]]; then
    ptrace_scope="$(cat /proc/sys/kernel/yama/ptrace_scope)"
  fi

  if [[ "${has_ptrace_cap}" -eq 1 || "${ptrace_scope}" == "0" ]]; then
    effective_ptrace_compat="0"
  else
    effective_ptrace_compat="1"
    echo "[run_experiment] ptrace prerequisites not met; enabling compatibility mode (PR_SET_PTRACER_ANY)."
    echo "[run_experiment] For realistic mode, run:"
    echo "  sudo setcap cap_sys_ptrace+ep ${ROOT_DIR}/bin/attacker_bias"
  fi
fi

timestamp="$(date +%Y%m%d_%H%M%S)"
run_dir="${OUT_BASE_DIR}/${timestamp}_${MODE}"
mkdir -p "${run_dir}"

if [[ -z "${META_FILE}" ]]; then
  META_FILE="${run_dir}/cps_controller_meta.txt"
fi

controller_log="${run_dir}/controller.log"
sensor_log="${run_dir}/sensor.log"
attacker_log="${run_dir}/attacker.log"
run_info="${run_dir}/run_info.txt"

controller_pid=""
sensor_pid=""
attacker_pid=""

cleanup() {
  set +e
  [[ -n "${attacker_pid}" ]] && kill "${attacker_pid}" 2>/dev/null
  [[ -n "${sensor_pid}" ]] && kill "${sensor_pid}" 2>/dev/null
  [[ -n "${controller_pid}" ]] && kill "${controller_pid}" 2>/dev/null
  wait "${attacker_pid}" 2>/dev/null || true
  wait "${sensor_pid}" 2>/dev/null || true
  wait "${controller_pid}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

cat >"${run_info}" <<EOF_INFO
mode=${MODE}
start_time=${timestamp}
duration_sec=${DURATION_SEC}
meta_file=${META_FILE}
setpoint=${SETPOINT}
kp=${KP}
controller_window_ms=${CONTROLLER_WINDOW_MS}
sensor_base=${SENSOR_BASE}
sensor_amp=${SENSOR_AMP}
sensor_interval_ms=${SENSOR_INTERVAL_MS}
warmup_sec=${WARMUP_SEC}
attack_bias=${ATTACK_BIAS}
attack_interval_ms=${ATTACK_INTERVAL_MS}
attack_rounds=${ATTACK_ROUNDS}
attack_symbol=${ATTACK_SYMBOL}
attack_proc_name=${ATTACK_PROC_NAME}
ptrace_compat=${effective_ptrace_compat}
EOF_INFO

echo "[run_experiment] run_dir=${run_dir}"
echo "[run_experiment] mode=${MODE}"

rm -f "${META_FILE}"
CPS_META_FILE="${META_FILE}" CPS_PTRACE_COMPAT="${effective_ptrace_compat}" \
  "${ROOT_DIR}/bin/controller" "${SETPOINT}" "${KP}" "${CONTROLLER_WINDOW_MS}" >"${controller_log}" 2>&1 &
controller_pid=$!
echo "[run_experiment] controller pid=${controller_pid}"

for _ in $(seq 1 50); do
  if [[ -f "${META_FILE}" ]]; then
    break
  fi
  if ! kill -0 "${controller_pid}" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

if [[ ! -f "${META_FILE}" ]]; then
  echo "[run_experiment] controller metadata not found: ${META_FILE}" >&2
  if ! kill -0 "${controller_pid}" 2>/dev/null; then
    echo "[run_experiment] controller exited before metadata creation" >&2
  else
    echo "[run_experiment] controller still running but metadata file absent" >&2
  fi
  echo "[run_experiment] last controller log lines:" >&2
  tail -n 80 "${controller_log}" >&2 || true
  exit 1
fi

"${ROOT_DIR}/bin/sensor" "${SENSOR_BASE}" "${SENSOR_AMP}" "${SENSOR_INTERVAL_MS}" >"${sensor_log}" 2>&1 &
sensor_pid=$!
echo "[run_experiment] sensor pid=${sensor_pid}"

if [[ "${MODE}" == "bias" ]]; then
  sleep "${WARMUP_SEC}"
  "${ROOT_DIR}/scripts/run_bias_attack.sh" \
    "${META_FILE}" \
    "${ATTACK_BIAS}" \
    "${ATTACK_INTERVAL_MS}" \
    "${ATTACK_ROUNDS}" \
    "${ATTACK_SYMBOL}" \
    "${ATTACK_PROC_NAME}" \
    "${controller_pid}" >"${attacker_log}" 2>&1 &
  attacker_pid=$!
  echo "[run_experiment] attacker pid=${attacker_pid}"
else
  echo "[run_experiment] baseline mode: attacker not started"
  : >"${attacker_log}"
fi

echo "[run_experiment] running for ${DURATION_SEC}s ..."
sleep "${DURATION_SEC}"

echo "[run_experiment] complete"
echo "[run_experiment] logs:"
echo "  ${controller_log}"
echo "  ${sensor_log}"
echo "  ${attacker_log}"
echo "  ${run_info}"
