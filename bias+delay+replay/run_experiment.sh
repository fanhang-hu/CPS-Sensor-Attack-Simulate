#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

MODE="baseline"
DURATION_SEC=30
OUT_BASE_DIR="${ROOT_DIR}/logs"
META_FILE=""

SCENARIO="wheel"

SETPOINT="20"
KP="0.8"
CONTROLLER_TIMEOUT_MS="10"
CONTROLLER_PERIOD_MS="10"

SENSOR_BASE="20"
SENSOR_AMP="0.2"
SENSOR_INTERVAL_MS="100"

ACTUATOR_MAX_TORQUE="12"
ACTUATOR_ALPHA="0.65"

PLANT_INIT_SPEED="20"
PLANT_STEP_MS="50"
PLANT_DRIVE_GAIN="0.42"
PLANT_DAMPING="0.18"

WARMUP_SEC=2
ATTACK_BIAS="4.0"
ATTACK_INTERVAL_MS="100"
ATTACK_ROUNDS="120"
ATTACK_SYMBOL="g_latest_measurement"
ATTACK_PROC_NAME="controller"
ATTACK_DELAY_HOLD_MS="180"
ATTACK_DELAY_INTERVAL_MS="250"
ATTACK_DELAY_ROUNDS="80"
ATTACK_DELAY_TARGET_PROC="sensor"
ATTACK_DELAY_JITTER_MS="30"
ATTACK_REPLAY_HOLD_MS="220"
ATTACK_REPLAY_INTERVAL_MS="320"
ATTACK_REPLAY_ROUNDS="50"
ATTACK_REPLAY_TARGET_PROC="sensor"
ATTACK_REPLAY_SEND_INTERVAL_MS="40"
ATTACK_REPLAY_CAPTURE_MIN="20"
ATTACK_REPLAY_WINDOW="12"
ATTACK_REPLAY_JITTER_MS="20"
PTRACE_COMPAT="auto"
PRIV_ESC_SOCKET="/tmp/cps_maintd.sock"
PRIV_ESC_CLIENT="${ROOT_DIR}/bin/cps_maint_client"
PRIV_ESC_TARGET="${ROOT_DIR}/bin/attacker_bias"

usage() {
  cat <<'USAGE_EOF'
Usage:
  ./scripts/run_experiment.sh [options]

Options:
  --mode baseline|bias|delay|replay|fullchain
                              baseline: no attacker
                              bias: run memory-tamper attacker directly
                              delay: run timing attacker via SIGSTOP/SIGCONT
                              replay: pause sensor and replay stale sensor frames
                              fullchain: abuse misconfigured root maintenance daemon, then attack
  --duration-sec N            Total run time in seconds (default: 30)
  --out-dir DIR               Output base directory (default: ./logs)
  --meta-file PATH            Controller metadata file (default: <run_dir>/cps_controller_meta.txt)
  --scenario wheel|simple     wheel=closed-loop CPS, simple=legacy sensor->controller only

  --setpoint V                Controller setpoint (default: 20)
  --kp V                      Controller Kp (default: 0.8)
  --controller-timeout-ms N   Controller select timeout in ms (default: 10)
  --controller-window-ms N    Backward-compatible alias of --controller-timeout-ms
  --controller-period-ms N    Controller scan/control period in ms (default: 10)

  --sensor-base V             Sensor base value in simple mode (default: 20)
  --sensor-amp V              Sensor amplitude / measurement noise amp (default: 0.2)
  --sensor-interval-ms N      Sensor send interval in ms (default: 100)

  --actuator-max-torque V     Actuator saturation abs bound (default: 12)
  --actuator-alpha V          Actuator first-order lag coefficient [0,1] (default: 0.65)

  --plant-init-speed V        Plant initial wheel speed (default: 20)
  --plant-step-ms N           Plant integration step in ms (default: 50)
  --plant-drive-gain V        Plant drive gain (default: 0.42)
  --plant-damping V           Plant damping (default: 0.18)

  --warmup-sec N              Seconds before launching attacker (default: 2)
  --attack-bias V             Added bias each attack round (default: 4.0)
  --attack-interval-ms N      Attack interval in ms (default: 100)
  --attack-rounds N           Attack rounds (default: 120)
  --attack-symbol NAME        Target symbol name (default: g_latest_measurement)
  --attack-proc-name NAME     Target process name for auto mode (default: controller)
  --delay-hold-ms N           Pause duration per round in ms (default: 180)
  --delay-interval-ms N       Interval between pauses in ms (default: 250)
  --delay-rounds N            Delay attack rounds (default: 80)
  --delay-target NAME         Delay target process name (default: sensor)
  --delay-jitter-ms N         Random jitter in ms for hold/interval (default: 30)
  --replay-hold-ms N          Replay pause duration per round in ms (default: 220)
  --replay-interval-ms N      Replay interval between rounds in ms (default: 320)
  --replay-rounds N           Replay attack rounds (default: 50)
  --replay-target NAME        Replay target process name (default: sensor)
  --replay-send-interval-ms N Replay packet send interval in ms while paused (default: 40)
  --replay-capture-min N      Min captured sensor frames before replay (default: 20)
  --replay-window N           Stale replay window size in frames (default: 12)
  --replay-jitter-ms N        Random jitter in ms for hold/interval (default: 20)
  --ptrace-compat MODE        auto|0|1. 1 enables PR_SET_PTRACER_ANY in controller
  --priv-esc-socket PATH      Unix socket of privileged maintenance daemon (default: /tmp/cps_maintd.sock)
  --priv-esc-client PATH      Escalation client binary (default: ./bin/cps_maint_client)
  --priv-esc-target PATH      Binary to grant capability to (default: ./bin/attacker_bias)
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
    --scenario)
      SCENARIO="${2:-}"
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
    --controller-timeout-ms|--controller-window-ms)
      CONTROLLER_TIMEOUT_MS="${2:-}"
      shift 2
      ;;
    --controller-period-ms)
      CONTROLLER_PERIOD_MS="${2:-}"
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
    --actuator-max-torque)
      ACTUATOR_MAX_TORQUE="${2:-}"
      shift 2
      ;;
    --actuator-alpha)
      ACTUATOR_ALPHA="${2:-}"
      shift 2
      ;;
    --plant-init-speed)
      PLANT_INIT_SPEED="${2:-}"
      shift 2
      ;;
    --plant-step-ms)
      PLANT_STEP_MS="${2:-}"
      shift 2
      ;;
    --plant-drive-gain)
      PLANT_DRIVE_GAIN="${2:-}"
      shift 2
      ;;
    --plant-damping)
      PLANT_DAMPING="${2:-}"
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
    --delay-hold-ms)
      ATTACK_DELAY_HOLD_MS="${2:-}"
      shift 2
      ;;
    --delay-interval-ms)
      ATTACK_DELAY_INTERVAL_MS="${2:-}"
      shift 2
      ;;
    --delay-rounds)
      ATTACK_DELAY_ROUNDS="${2:-}"
      shift 2
      ;;
    --delay-target)
      ATTACK_DELAY_TARGET_PROC="${2:-}"
      shift 2
      ;;
    --delay-jitter-ms)
      ATTACK_DELAY_JITTER_MS="${2:-}"
      shift 2
      ;;
    --replay-hold-ms)
      ATTACK_REPLAY_HOLD_MS="${2:-}"
      shift 2
      ;;
    --replay-interval-ms)
      ATTACK_REPLAY_INTERVAL_MS="${2:-}"
      shift 2
      ;;
    --replay-rounds)
      ATTACK_REPLAY_ROUNDS="${2:-}"
      shift 2
      ;;
    --replay-target)
      ATTACK_REPLAY_TARGET_PROC="${2:-}"
      shift 2
      ;;
    --replay-send-interval-ms)
      ATTACK_REPLAY_SEND_INTERVAL_MS="${2:-}"
      shift 2
      ;;
    --replay-capture-min)
      ATTACK_REPLAY_CAPTURE_MIN="${2:-}"
      shift 2
      ;;
    --replay-window)
      ATTACK_REPLAY_WINDOW="${2:-}"
      shift 2
      ;;
    --replay-jitter-ms)
      ATTACK_REPLAY_JITTER_MS="${2:-}"
      shift 2
      ;;
    --ptrace-compat)
      PTRACE_COMPAT="${2:-}"
      shift 2
      ;;
    --priv-esc-socket)
      PRIV_ESC_SOCKET="${2:-}"
      shift 2
      ;;
    --priv-esc-client)
      PRIV_ESC_CLIENT="${2:-}"
      shift 2
      ;;
    --priv-esc-target)
      PRIV_ESC_TARGET="${2:-}"
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

if [[ "${MODE}" != "baseline" && "${MODE}" != "bias" && "${MODE}" != "delay" && "${MODE}" != "replay" && "${MODE}" != "fullchain" ]]; then
  echo "[run_experiment] --mode must be baseline, bias, delay, replay, or fullchain" >&2
  exit 1
fi

if [[ "${SCENARIO}" != "wheel" && "${SCENARIO}" != "simple" ]]; then
  echo "[run_experiment] --scenario must be wheel or simple" >&2
  exit 1
fi

if [[ "${PTRACE_COMPAT}" != "auto" && "${PTRACE_COMPAT}" != "0" && "${PTRACE_COMPAT}" != "1" ]]; then
  echo "[run_experiment] --ptrace-compat must be auto, 0, or 1" >&2
  exit 1
fi

if ! [[ "${CONTROLLER_TIMEOUT_MS}" =~ ^[0-9]+$ ]]; then
  echo "[run_experiment] --controller-timeout-ms must be a non-negative integer" >&2
  exit 1
fi

if ! [[ "${CONTROLLER_PERIOD_MS}" =~ ^[0-9]+$ ]] || [[ "${CONTROLLER_PERIOD_MS}" -le 0 ]]; then
  echo "[run_experiment] --controller-period-ms must be a positive integer" >&2
  exit 1
fi

if ! [[ "${ATTACK_DELAY_HOLD_MS}" =~ ^[0-9]+$ ]] || [[ "${ATTACK_DELAY_HOLD_MS}" -le 0 ]]; then
  echo "[run_experiment] --delay-hold-ms must be a positive integer" >&2
  exit 1
fi

if ! [[ "${ATTACK_DELAY_INTERVAL_MS}" =~ ^[0-9]+$ ]] || [[ "${ATTACK_DELAY_INTERVAL_MS}" -le 0 ]]; then
  echo "[run_experiment] --delay-interval-ms must be a positive integer" >&2
  exit 1
fi

if ! [[ "${ATTACK_DELAY_ROUNDS}" =~ ^[0-9]+$ ]] || [[ "${ATTACK_DELAY_ROUNDS}" -le 0 ]]; then
  echo "[run_experiment] --delay-rounds must be a positive integer" >&2
  exit 1
fi

if ! [[ "${ATTACK_DELAY_JITTER_MS}" =~ ^[0-9]+$ ]]; then
  echo "[run_experiment] --delay-jitter-ms must be a non-negative integer" >&2
  exit 1
fi

if [[ -z "${ATTACK_DELAY_TARGET_PROC}" ]]; then
  echo "[run_experiment] --delay-target must not be empty" >&2
  exit 1
fi

if ! [[ "${ATTACK_REPLAY_HOLD_MS}" =~ ^[0-9]+$ ]] || [[ "${ATTACK_REPLAY_HOLD_MS}" -le 0 ]]; then
  echo "[run_experiment] --replay-hold-ms must be a positive integer" >&2
  exit 1
fi

if ! [[ "${ATTACK_REPLAY_INTERVAL_MS}" =~ ^[0-9]+$ ]] || [[ "${ATTACK_REPLAY_INTERVAL_MS}" -le 0 ]]; then
  echo "[run_experiment] --replay-interval-ms must be a positive integer" >&2
  exit 1
fi

if ! [[ "${ATTACK_REPLAY_ROUNDS}" =~ ^[0-9]+$ ]] || [[ "${ATTACK_REPLAY_ROUNDS}" -le 0 ]]; then
  echo "[run_experiment] --replay-rounds must be a positive integer" >&2
  exit 1
fi

if ! [[ "${ATTACK_REPLAY_SEND_INTERVAL_MS}" =~ ^[0-9]+$ ]] || [[ "${ATTACK_REPLAY_SEND_INTERVAL_MS}" -le 0 ]]; then
  echo "[run_experiment] --replay-send-interval-ms must be a positive integer" >&2
  exit 1
fi

if ! [[ "${ATTACK_REPLAY_CAPTURE_MIN}" =~ ^[0-9]+$ ]] || [[ "${ATTACK_REPLAY_CAPTURE_MIN}" -le 0 ]]; then
  echo "[run_experiment] --replay-capture-min must be a positive integer" >&2
  exit 1
fi

if ! [[ "${ATTACK_REPLAY_WINDOW}" =~ ^[0-9]+$ ]] || [[ "${ATTACK_REPLAY_WINDOW}" -le 0 ]]; then
  echo "[run_experiment] --replay-window must be a positive integer" >&2
  exit 1
fi

if ! [[ "${ATTACK_REPLAY_JITTER_MS}" =~ ^[0-9]+$ ]]; then
  echo "[run_experiment] --replay-jitter-ms must be a non-negative integer" >&2
  exit 1
fi

if [[ -z "${ATTACK_REPLAY_TARGET_PROC}" ]]; then
  echo "[run_experiment] --replay-target must not be empty" >&2
  exit 1
fi

if [[ -z "${PRIV_ESC_SOCKET}" ]]; then
  echo "[run_experiment] --priv-esc-socket must not be empty" >&2
  exit 1
fi

if [[ -z "${PRIV_ESC_CLIENT}" ]]; then
  echo "[run_experiment] --priv-esc-client must not be empty" >&2
  exit 1
fi

if [[ -z "${PRIV_ESC_TARGET}" ]]; then
  echo "[run_experiment] --priv-esc-target must not be empty" >&2
  exit 1
fi

has_ptrace_cap() {
  if ! command -v getcap >/dev/null 2>&1; then
    return 1
  fi
  getcap "${ROOT_DIR}/bin/attacker_bias" 2>/dev/null | grep -q "cap_sys_ptrace"
}

need_rebuild=0
required_bins=(controller sensor attacker_bias attacker_delay attacker_replay)
if [[ "${SCENARIO}" == "wheel" ]]; then
  required_bins+=(actuator plant)
fi
if [[ "${MODE}" == "fullchain" ]]; then
  required_bins+=(cps_maint_client)
fi

for b in "${required_bins[@]}"; do
  if [[ ! -x "${ROOT_DIR}/bin/${b}" ]]; then
    need_rebuild=1
  fi
done

if [[ "$(uname -s)" == "Linux" ]] && command -v file >/dev/null 2>&1; then
  for b in "${required_bins[@]}"; do
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

timestamp="$(date +%Y%m%d_%H%M%S)"
run_dir="${OUT_BASE_DIR}/${timestamp}_${SCENARIO}_${MODE}"
mkdir -p "${run_dir}"

if [[ -z "${META_FILE}" ]]; then
  META_FILE="${run_dir}/cps_controller_meta.txt"
fi

controller_log="${run_dir}/controller.log"
sensor_log="${run_dir}/sensor.log"
attacker_log="${run_dir}/attacker.log"
actuator_log="${run_dir}/actuator.log"
plant_log="${run_dir}/plant.log"
priv_esc_log="${run_dir}/priv_esc.log"
run_info="${run_dir}/run_info.txt"

controller_pid=""
sensor_pid=""
attacker_pid=""
actuator_pid=""
plant_pid=""
run_failed=0
failure_reason=""

run_fullchain_priv_esc() {
  : >"${priv_esc_log}"
  echo "[priv_esc] socket=${PRIV_ESC_SOCKET}" | tee -a "${priv_esc_log}"
  echo "[priv_esc] client=${PRIV_ESC_CLIENT}" | tee -a "${priv_esc_log}"
  echo "[priv_esc] target=${PRIV_ESC_TARGET}" | tee -a "${priv_esc_log}"

  if has_ptrace_cap; then
    echo "[priv_esc] cap_sys_ptrace already present, skip escalation request." | tee -a "${priv_esc_log}"
    return 0
  fi

  if [[ ! -x "${PRIV_ESC_CLIENT}" ]]; then
    echo "[priv_esc] escalation client not executable: ${PRIV_ESC_CLIENT}" | tee -a "${priv_esc_log}"
    return 1
  fi

  if [[ ! -S "${PRIV_ESC_SOCKET}" ]]; then
    echo "[priv_esc] maintenance socket not found: ${PRIV_ESC_SOCKET}" | tee -a "${priv_esc_log}"
    echo "[priv_esc] start daemon first, e.g.: sudo ${ROOT_DIR}/bin/cps_maintd ${PRIV_ESC_SOCKET}" | tee -a "${priv_esc_log}"
    return 1
  fi

  echo "[priv_esc] running client request to maintenance daemon" | tee -a "${priv_esc_log}"
  if ! "${PRIV_ESC_CLIENT}" "${PRIV_ESC_SOCKET}" "${PRIV_ESC_TARGET}" >>"${priv_esc_log}" 2>&1; then
    echo "[priv_esc] daemon request failed." | tee -a "${priv_esc_log}"
    return 1
  fi

  if has_ptrace_cap; then
    echo "[priv_esc] capability grant succeeded." | tee -a "${priv_esc_log}"
    return 0
  fi

  echo "[priv_esc] escalation request completed but cap_sys_ptrace still missing." | tee -a "${priv_esc_log}"
  return 1
}

if [[ "${MODE}" == "fullchain" ]]; then
  if ! run_fullchain_priv_esc; then
    echo "[run_experiment] fullchain privilege escalation failed. See: ${priv_esc_log}" >&2
    tail -n 80 "${priv_esc_log}" >&2 || true
    exit 1
  fi
else
  : >"${priv_esc_log}"
fi

effective_ptrace_compat="${PTRACE_COMPAT}"
if [[ ( "${MODE}" == "bias" || "${MODE}" == "fullchain" ) && "${PTRACE_COMPAT}" == "auto" ]]; then
  has_ptrace_cap_flag=0
  if has_ptrace_cap; then
    has_ptrace_cap_flag=1
  fi

  ptrace_scope="1"
  if [[ -r /proc/sys/kernel/yama/ptrace_scope ]]; then
    ptrace_scope="$(cat /proc/sys/kernel/yama/ptrace_scope)"
  fi

  if [[ "${has_ptrace_cap_flag}" -eq 1 || "${ptrace_scope}" == "0" ]]; then
    effective_ptrace_compat="0"
  else
    effective_ptrace_compat="1"
    echo "[run_experiment] ptrace prerequisites not met; enabling compatibility mode (PR_SET_PTRACER_ANY)."
    echo "[run_experiment] For realistic mode, run:"
    echo "  sudo setcap cap_sys_ptrace+ep ${ROOT_DIR}/bin/attacker_bias"
  fi
fi

cleanup() {
  set +e
  [[ -n "${attacker_pid}" ]] && kill "${attacker_pid}" 2>/dev/null
  [[ -n "${sensor_pid}" ]] && kill "${sensor_pid}" 2>/dev/null
  [[ -n "${controller_pid}" ]] && kill "${controller_pid}" 2>/dev/null
  [[ -n "${actuator_pid}" ]] && kill "${actuator_pid}" 2>/dev/null
  [[ -n "${plant_pid}" ]] && kill "${plant_pid}" 2>/dev/null
  wait "${attacker_pid}" 2>/dev/null || true
  wait "${sensor_pid}" 2>/dev/null || true
  wait "${controller_pid}" 2>/dev/null || true
  wait "${actuator_pid}" 2>/dev/null || true
  wait "${plant_pid}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

pid_alive() {
  local pid="${1:-}"
  [[ -n "${pid}" ]] || return 1
  kill -0 "${pid}" 2>/dev/null
}

fail_with_log() {
  local reason="$1"
  local logfile="$2"
  run_failed=1
  failure_reason="${reason}"
  echo "[run_experiment] ERROR: ${reason}" >&2
  if [[ -f "${logfile}" ]]; then
    echo "[run_experiment] last lines from ${logfile}:" >&2
    tail -n 80 "${logfile}" >&2 || true
  fi
}

cat >"${run_info}" <<EOF_INFO
mode=${MODE}
scenario=${SCENARIO}
start_time=${timestamp}
duration_sec=${DURATION_SEC}
meta_file=${META_FILE}
setpoint=${SETPOINT}
kp=${KP}
controller_window_ms=${CONTROLLER_TIMEOUT_MS}
controller_poll_timeout_ms=${CONTROLLER_TIMEOUT_MS}
controller_period_ms=${CONTROLLER_PERIOD_MS}
sensor_base=${SENSOR_BASE}
sensor_amp=${SENSOR_AMP}
sensor_interval_ms=${SENSOR_INTERVAL_MS}
actuator_max_torque=${ACTUATOR_MAX_TORQUE}
actuator_alpha=${ACTUATOR_ALPHA}
plant_init_speed=${PLANT_INIT_SPEED}
plant_step_ms=${PLANT_STEP_MS}
plant_drive_gain=${PLANT_DRIVE_GAIN}
plant_damping=${PLANT_DAMPING}
warmup_sec=${WARMUP_SEC}
attack_bias=${ATTACK_BIAS}
attack_interval_ms=${ATTACK_INTERVAL_MS}
attack_rounds=${ATTACK_ROUNDS}
attack_symbol=${ATTACK_SYMBOL}
attack_proc_name=${ATTACK_PROC_NAME}
delay_hold_ms=${ATTACK_DELAY_HOLD_MS}
delay_interval_ms=${ATTACK_DELAY_INTERVAL_MS}
delay_rounds=${ATTACK_DELAY_ROUNDS}
delay_target_proc=${ATTACK_DELAY_TARGET_PROC}
delay_jitter_ms=${ATTACK_DELAY_JITTER_MS}
replay_hold_ms=${ATTACK_REPLAY_HOLD_MS}
replay_interval_ms=${ATTACK_REPLAY_INTERVAL_MS}
replay_rounds=${ATTACK_REPLAY_ROUNDS}
replay_target_proc=${ATTACK_REPLAY_TARGET_PROC}
replay_send_interval_ms=${ATTACK_REPLAY_SEND_INTERVAL_MS}
replay_capture_min=${ATTACK_REPLAY_CAPTURE_MIN}
replay_window=${ATTACK_REPLAY_WINDOW}
replay_jitter_ms=${ATTACK_REPLAY_JITTER_MS}
ptrace_compat=${effective_ptrace_compat}
priv_esc_socket=${PRIV_ESC_SOCKET}
priv_esc_client=${PRIV_ESC_CLIENT}
priv_esc_target=${PRIV_ESC_TARGET}
EOF_INFO

echo "[run_experiment] run_dir=${run_dir}"
echo "[run_experiment] mode=${MODE} scenario=${SCENARIO}"

if [[ "${SCENARIO}" == "wheel" ]]; then
  "${ROOT_DIR}/bin/plant" \
    "${PLANT_INIT_SPEED}" \
    "${PLANT_STEP_MS}" \
    "${PLANT_DRIVE_GAIN}" \
    "${PLANT_DAMPING}" >"${plant_log}" 2>&1 &
  plant_pid=$!
  echo "[run_experiment] plant pid=${plant_pid}"

  "${ROOT_DIR}/bin/actuator" \
    "${ACTUATOR_MAX_TORQUE}" \
    "${ACTUATOR_ALPHA}" >"${actuator_log}" 2>&1 &
  actuator_pid=$!
  echo "[run_experiment] actuator pid=${actuator_pid}"
else
  : >"${plant_log}"
  : >"${actuator_log}"
fi

rm -f "${META_FILE}"
CPS_META_FILE="${META_FILE}" \
CPS_PTRACE_COMPAT="${effective_ptrace_compat}" \
CPS_ACTUATOR_ENABLE="$([[ "${SCENARIO}" == "wheel" ]] && echo 1 || echo 0)" \
  "${ROOT_DIR}/bin/controller" "${SETPOINT}" "${KP}" "${CONTROLLER_TIMEOUT_MS}" "${CONTROLLER_PERIOD_MS}" >"${controller_log}" 2>&1 &
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

if [[ "${SCENARIO}" == "wheel" ]]; then
  CPS_SENSOR_MODE="plant" \
    "${ROOT_DIR}/bin/sensor" "${SENSOR_BASE}" "${SENSOR_AMP}" "${SENSOR_INTERVAL_MS}" >"${sensor_log}" 2>&1 &
else
  "${ROOT_DIR}/bin/sensor" "${SENSOR_BASE}" "${SENSOR_AMP}" "${SENSOR_INTERVAL_MS}" >"${sensor_log}" 2>&1 &
fi
sensor_pid=$!
echo "[run_experiment] sensor pid=${sensor_pid}"

sleep 1
if ! pid_alive "${controller_pid}"; then
  fail_with_log "controller crashed during startup" "${controller_log}"
  exit 1
fi
if ! pid_alive "${sensor_pid}"; then
  fail_with_log "sensor crashed during startup" "${sensor_log}"
  exit 1
fi
if [[ "${SCENARIO}" == "wheel" ]]; then
  if ! pid_alive "${plant_pid}"; then
    fail_with_log "plant crashed during startup" "${plant_log}"
    exit 1
  fi
  if ! pid_alive "${actuator_pid}"; then
    fail_with_log "actuator crashed during startup" "${actuator_log}"
    exit 1
  fi
fi

if [[ "${MODE}" == "bias" || "${MODE}" == "fullchain" ]]; then
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
elif [[ "${MODE}" == "delay" ]]; then
  sleep "${WARMUP_SEC}"
  delay_target_spec="auto"
  if [[ "${ATTACK_DELAY_TARGET_PROC}" == "sensor" && -n "${sensor_pid}" ]]; then
    delay_target_spec="${sensor_pid}"
  elif [[ "${ATTACK_DELAY_TARGET_PROC}" == "controller" && -n "${controller_pid}" ]]; then
    delay_target_spec="${controller_pid}"
  elif [[ "${ATTACK_DELAY_TARGET_PROC}" == "actuator" && -n "${actuator_pid}" ]]; then
    delay_target_spec="${actuator_pid}"
  elif [[ "${ATTACK_DELAY_TARGET_PROC}" == "plant" && -n "${plant_pid}" ]]; then
    delay_target_spec="${plant_pid}"
  fi

  "${ROOT_DIR}/scripts/run_delay_attack.sh" \
    "${META_FILE}" \
    "${ATTACK_DELAY_HOLD_MS}" \
    "${ATTACK_DELAY_INTERVAL_MS}" \
    "${ATTACK_DELAY_ROUNDS}" \
    "${ATTACK_DELAY_TARGET_PROC}" \
    "${ATTACK_DELAY_JITTER_MS}" \
    "${delay_target_spec}" >"${attacker_log}" 2>&1 &
  attacker_pid=$!
  echo "[run_experiment] attacker pid=${attacker_pid} delay_target_spec=${delay_target_spec}"
elif [[ "${MODE}" == "replay" ]]; then
  sleep "${WARMUP_SEC}"
  replay_target_spec="auto"
  if [[ "${ATTACK_REPLAY_TARGET_PROC}" == "sensor" && -n "${sensor_pid}" ]]; then
    replay_target_spec="${sensor_pid}"
  elif [[ "${ATTACK_REPLAY_TARGET_PROC}" == "controller" && -n "${controller_pid}" ]]; then
    replay_target_spec="${controller_pid}"
  elif [[ "${ATTACK_REPLAY_TARGET_PROC}" == "actuator" && -n "${actuator_pid}" ]]; then
    replay_target_spec="${actuator_pid}"
  elif [[ "${ATTACK_REPLAY_TARGET_PROC}" == "plant" && -n "${plant_pid}" ]]; then
    replay_target_spec="${plant_pid}"
  fi

  "${ROOT_DIR}/scripts/run_replay_attack.sh" \
    "${META_FILE}" \
    "${sensor_log}" \
    "${ATTACK_REPLAY_HOLD_MS}" \
    "${ATTACK_REPLAY_INTERVAL_MS}" \
    "${ATTACK_REPLAY_ROUNDS}" \
    "${ATTACK_REPLAY_TARGET_PROC}" \
    "${ATTACK_REPLAY_SEND_INTERVAL_MS}" \
    "${ATTACK_REPLAY_CAPTURE_MIN}" \
    "${ATTACK_REPLAY_WINDOW}" \
    "${ATTACK_REPLAY_JITTER_MS}" \
    "${replay_target_spec}" >"${attacker_log}" 2>&1 &
  attacker_pid=$!
  echo "[run_experiment] attacker pid=${attacker_pid} replay_target_spec=${replay_target_spec}"
else
  echo "[run_experiment] baseline mode: attacker not started"
  : >"${attacker_log}"
fi

echo "[run_experiment] running for ${DURATION_SEC}s ..."
elapsed=0
while [[ "${elapsed}" -lt "${DURATION_SEC}" ]]; do
  sleep 1
  elapsed=$((elapsed + 1))

  if ! pid_alive "${controller_pid}"; then
    fail_with_log "controller crashed while running" "${controller_log}"
    break
  fi
  if ! pid_alive "${sensor_pid}"; then
    fail_with_log "sensor crashed while running" "${sensor_log}"
    break
  fi
  if [[ "${SCENARIO}" == "wheel" ]]; then
    if ! pid_alive "${plant_pid}"; then
      fail_with_log "plant crashed while running" "${plant_log}"
      break
    fi
    if ! pid_alive "${actuator_pid}"; then
      fail_with_log "actuator crashed while running" "${actuator_log}"
      break
    fi
  fi
done

echo "[run_experiment] complete"
echo "[run_experiment] logs:"
echo "  ${controller_log}"
echo "  ${sensor_log}"
echo "  ${attacker_log}"
echo "  ${actuator_log}"
echo "  ${plant_log}"
echo "  ${priv_esc_log}"
echo "  ${run_info}"

if [[ "${run_failed}" -eq 1 ]]; then
  echo "[run_experiment] failed: ${failure_reason}" >&2
  exit 1
fi
