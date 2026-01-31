- **pass**: `sensor -> controller` direct (no MITM on the measurement path)
- **bias**: `sensor -> mitm -> controller` where MITM adds a constant bias to the measurement

Fix the **sensor source port** so that, in `pass`, controller always receives from the same `(src_port)`. In `bias`, controller receives from a different source port (because MITM forwards), i.e., a **new writer on the channel** appears. This is observable
purely from socket syscalls (`recvfrom/sendto`) without keying on the MITM executable name.

- `plant.py`      : plant, receives control `u` on 9002, sends state `x` to sensor on 9000
- `sensor.py`     : receives `x`, sends measurement `y` with fixed **source port** (default 9100)
- `mitm.c`        : in **bias** mode, listens on 9001 and forwards to controller 9003 with `y += bias`
- `controller.py` : receives `y` on 9003, computes `u`, sends to plant on 9002

```bash
# PASS (no MITM on measurement path)
./run_demo.sh pass 30

# BIAS (MITM on measurement path)
BIAS=1.5 ./run_demo.sh bias 30
```

- `SENSOR_TX_PORT` (default 9100): fixed UDP source port for `sensor -> {controller|mitm}`
- `NOISE_STD` (default 0.0): set to >0 if you want measurement noise
