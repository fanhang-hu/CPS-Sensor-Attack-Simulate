Terminal A,
```
sudo insmod ./nodrop.ko
```
Terminal B, fullchain,
```
./scripts/run_experiment.sh --mode fullchain --duration-sec 30 --scenario wheel
```
delay,
```
./scripts/run_experiment.sh --mode delay --duration-sec 30 --scenario wheel \
  --delay-target sensor --delay-hold-ms 180 --delay-interval-ms 250 --delay-rounds 80
```
replay,
```
./scripts/run_experiment.sh --mode replay --duration-sec 40 --scenario wheel \
  --setpoint 20 --plant-init-speed 8 \
  --replay-target sensor --replay-hold-ms 220 --replay-interval-ms 320 \
  --replay-rounds 50 --replay-send-interval-ms 40 \
  --replay-capture-min 20 --replay-window 12
```
