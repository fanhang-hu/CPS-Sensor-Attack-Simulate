#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-pass}"
shift || true

TAMPER="0"
if [[ "${1:-}" == "--tamper-gains" ]]; then
  TAMPER="1"
  shift || true
fi

mkdir -p /tmp/cps_bench_logs
rm -f /tmp/cps_bench_logs/*.csv /tmp/cps_bench_gains.json /tmp/cps_bench_attack_marker.txt

# Build MITM
gcc -O2 -Wall -o mitm mitm.c

# Start processes (background)
python3 plant.py      > /tmp/cps_bench_logs/plant.out 2>&1 &
P_PLANT=$!
python3 sensor.py     > /tmp/cps_bench_logs/sensor.out 2>&1 &
P_SENSOR=$!
./mitm --mode "$MODE" $( [[ "$TAMPER" == "1" ]] && echo "--tamper-gains" ) \
  > /tmp/cps_bench_logs/mitm.out 2>&1 &
P_MITM=$!
python3 controller.py > /tmp/cps_bench_logs/controller.out 2>&1 &
P_CTRL=$!

echo "[run_demo] plant=$P_PLANT sensor=$P_SENSOR mitm=$P_MITM controller=$P_CTRL"
echo "[run_demo] mode=$MODE tamper_gains=$TAMPER"
echo "[run_demo] logs: /tmp/cps_bench_logs/*.csv (and *.out)"
echo "[run_demo] press Ctrl+C to stop."

cleanup() {
  echo "[run_demo] stopping..."
  kill $P_CTRL $P_MITM $P_SENSOR $P_PLANT 2>/dev/null || true
}
trap cleanup INT TERM

# run for 30s by default (change if needed)
sleep 30
cleanup
