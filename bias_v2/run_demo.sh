#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-pass}"          # pass | bias
DUR="${2:-30}"             # seconds
shift 2 || true

BIAS="${BIAS:-1.5}"        # can override by env, or pass --bias to mitm
MITM_ARGS=("$@")           # extra args forwarded to mitm (e.g., --bias 2.0)

LOG_DIR="/tmp/cps_bias_v2_logs"
BUS_DIR="/tmp/cps_bias_v2_bus"
KEY_PATH="${BUS_DIR}/key.bin"

mkdir -p "${LOG_DIR}" "${BUS_DIR}"
rm -f "${LOG_DIR}"/*.csv "${LOG_DIR}"/*.out
rm -f "${BUS_DIR}"/*.pipe

# Create a key (only for integrity check; MITM doesn't know/recompute it in this demo)
if [[ ! -f "${KEY_PATH}" ]]; then
  # 32 bytes random key
  python3 - <<'PY'
import os, secrets
bus="/tmp/cps_bias_v2_bus"
os.makedirs(bus, exist_ok=True)
with open(os.path.join(bus,"key.bin"),"wb") as f:
    f.write(secrets.token_bytes(32))
print("[run_demo] created /tmp/cps_bias_v2_bus/key.bin (32 bytes)")
PY
fi

# Build MITM (only used in bias mode)
gcc -O2 -Wall -o mitm mitm.c

# Decide FIFO topology
if [[ "${MODE}" == "pass" ]]; then
  MEAS_PIPE="${BUS_DIR}/y.pipe"
  mkfifo "${MEAS_PIPE}"
  export MEAS_OUT="${MEAS_PIPE}"
  export MEAS_IN="${MEAS_PIPE}"
  echo "[run_demo] MODE=pass: sensor -> ${MEAS_PIPE} -> controller (no MITM)"
elif [[ "${MODE}" == "bias" ]]; then
  IN_PIPE="${BUS_DIR}/y_in.pipe"
  OUT_PIPE="${BUS_DIR}/y_out.pipe"
  mkfifo "${IN_PIPE}" "${OUT_PIPE}"
  export MEAS_OUT="${IN_PIPE}"
  export MEAS_IN="${OUT_PIPE}"
  echo "[run_demo] MODE=bias: sensor -> ${IN_PIPE} -> MITM(+bias) -> ${OUT_PIPE} -> controller"
else
  echo "Usage: $0 {pass|bias} [duration_sec] [--bias X]"
  exit 1
fi

export KEY_PATH="${KEY_PATH}"

# Start processes (background)
python3 plant.py      > "${LOG_DIR}/plant.out" 2>&1 &
P_PLANT=$!

# Controller first (so sensor FIFO open doesn't block forever)
python3 controller.py > "${LOG_DIR}/controller.out" 2>&1 &
P_CTRL=$!

if [[ "${MODE}" == "bias" ]]; then
  # MITM needs controller to open OUT fifo, and sensor to open IN fifo; order is ok
  ./mitm --in "${IN_PIPE}" --out "${OUT_PIPE}" --bias "${BIAS}" "${MITM_ARGS[@]}" \
    > "${LOG_DIR}/mitm.out" 2>&1 &
  P_MITM=$!
else
  P_MITM=""
fi

python3 sensor.py     > "${LOG_DIR}/sensor.out" 2>&1 &
P_SENSOR=$!

echo "[run_demo] plant=${P_PLANT} sensor=${P_SENSOR} controller=${P_CTRL} mitm=${P_MITM:-none}"
echo "[run_demo] logs: ${LOG_DIR} (csv + out)"
echo "[run_demo] running for ${DUR}s... Ctrl+C to stop."

cleanup() {
  echo "[run_demo] stopping..."
  kill ${P_SENSOR} ${P_CTRL} ${P_PLANT} 2>/dev/null || true
  if [[ -n "${P_MITM:-}" ]]; then
    kill ${P_MITM} 2>/dev/null || true
  fi
}
trap cleanup INT TERM

sleep "${DUR}"
cleanup
