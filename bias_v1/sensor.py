#!/usr/bin/env python3
import os, socket, struct, time, csv, random

# --- Configuration ---
# Plant -> Sensor (state x)
SENSOR_PORT = int(os.environ.get("SENSOR_PORT", "9000"))

# Sensor -> (Pass: Controller, Bias: MITM)
DST_PORT    = int(os.environ.get("DST_PORT", os.environ.get("MITM_PORT", "9001")))  # backward compatible
HOST = os.environ.get("HOST", "127.0.0.1")

# Route-A key: fix the UDP source port of the measurement stream
SENSOR_TX_PORT = int(os.environ.get("SENSOR_TX_PORT", "9100"))

# Optional measurement noise (set NOISE_STD=0 to disable)
NOISE_STD = float(os.environ.get("NOISE_STD", "0.0"))

FMT = "<d f I"
SIZE = struct.calcsize(FMT)

LOG_DIR = "/tmp/cps_bias_v1_logs"
os.makedirs(LOG_DIR, exist_ok=True)
LOG_PATH = os.path.join(LOG_DIR, "sensor.csv")

def main():
    # Receive plant state x[k]
    s_x = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s_x.bind((HOST, SENSOR_PORT))

    # Send measurement y[k] with a FIXED source port (Route-A)
    s_y = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s_y.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s_y.bind((HOST, SENSOR_TX_PORT))

    with open(LOG_PATH, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["x_true", "y_raw", "dst_port", "tx_src_port"])
        while True:
            data, _ = s_x.recvfrom(2048)
            if len(data) != SIZE:
                continue
            ts, x, seq = struct.unpack(FMT, data)

            # Measurement (optionally noisy)
            y = float(x) + (random.gauss(0.0, NOISE_STD) if NOISE_STD > 0 else 0.0)

            out = struct.pack(FMT, time.time(), float(y), int(seq))
            s_y.sendto(out, (HOST, DST_PORT))

            w.writerow([float(x), float(y), int(DST_PORT), int(SENSOR_TX_PORT)])
            if int(seq) % 50 == 0:
                f.flush()

if __name__ == "__main__":
    main()
