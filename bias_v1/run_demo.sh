#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-pass}"   # pass | bias
DURATION="${2:-30}" # seconds
HOST="127.0.0.1"

# Ports (keep consistent with demo)
PLANT_PORT="${PLANT_PORT:-9002}"
SENSOR_PORT="${SENSOR_PORT:-9000}"
CTRL_PORT="${CTRL_PORT:-9003}"
MITM_PORT="${MITM_PORT:-9001}"

# Route-A: fixed sensor TX source port
SENSOR_TX_PORT="${SENSOR_TX_PORT:-9100}"

# Bias value (used only in bias mode)
BIAS_VAL="${BIAS:-1.5}"

mkdir -p /tmp/cps_bias_v1_logs
rm -f /tmp/cps_bias_v1_logs/*.csv /tmp/cps_bias_v1_gains.json /tmp/cps_bias_v1_attack_marker.txt

# Build MITM (only needed for bias, but cheap to compile)
gcc -O2 -Wall -o mitm mitm.c

# Start plant + controller
python3 plant.py      > /tmp/cps_bias_v1_logs/plant.out 2>&1 &
P_PLANT=$!
python3 controller.py > /tmp/cps_bias_v1_logs/controller.out 2>&1 &
P_CTRL=$!

# Start sensor
if [[ "$MODE" == "pass" ]]; then
  # PASS: sensor sends directly to controller (DST_PORT = CTRL_PORT), NO MITM
  DST_PORT="$CTRL_PORT" SENSOR_TX_PORT="$SENSOR_TX_PORT" HOST="$HOST" \
    python3 sensor.py > /tmp/cps_bias_v1_logs/sensor.out 2>&1 &
  P_SENSOR=$!
  P_MITM=""
else
  # BIAS: sensor sends to MITM, MITM forwards to controller with +BIAS
  DST_PORT="$MITM_PORT" SENSOR_TX_PORT="$SENSOR_TX_PORT" HOST="$HOST" \
    python3 sensor.py > /tmp/cps_bias_v1_logs/sensor.out 2>&1 &
  P_SENSOR=$!

  ./mitm --mode bias --bias "$BIAS_VAL" > /tmp/cps_bias_v1_logs/mitm.out 2>&1 &
  P_MITM=$!
fi

echo "[run_demo] mode=$MODE duration=${DURATION}s"
echo "[run_demo] plant=$P_PLANT sensor=$P_SENSOR controller=$P_CTRL mitm=${P_MITM:-none}"
echo "[run_demo] ports: plant=$PLANT_PORT sensor=$SENSOR_PORT ctrl=$CTRL_PORT mitm=$MITM_PORT sensor_tx=$SENSOR_TX_PORT"
echo "[run_demo] logs: /tmp/cps_bias_v1_logs/*.csv (and *.out)"
echo "[run_demo] press Ctrl+C to stop."

cleanup() {
  echo "[run_demo] stopping..."
  [[ -n "${P_CTRL:-}"   ]] && kill "$P_CTRL"   2>/dev/null || true
  [[ -n "${P_MITM:-}"   ]] && kill "$P_MITM"   2>/dev/null || true
  [[ -n "${P_SENSOR:-}" ]] && kill "$P_SENSOR" 2>/dev/null || true
  [[ -n "${P_PLANT:-}"  ]] && kill "$P_PLANT"  2>/dev/null || true
}
trap cleanup INT TERM

sleep "$DURATION"
cleanup
