#!/usr/bin/env python3
import os, socket, struct, time, csv, random

# --- Configuration ---
SENSOR_PORT = int(os.environ.get("SENSOR_PORT", "9000"))
MITM_PORT   = int(os.environ.get("MITM_PORT", "9001"))
HOST = "127.0.0.1"
NOISE_STD = float(os.environ.get("NOISE_STD", "0.02"))

FMT = "<d f I"
SIZE = struct.calcsize(FMT)

LOG_DIR = "/tmp/cps_bench_logs"
os.makedirs(LOG_DIR, exist_ok=True)
LOG_PATH = os.path.join(LOG_DIR, "sensor.csv")

def main():
    s_x = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s_x.bind((HOST, SENSOR_PORT))

    s_y = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    with open(LOG_PATH, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t", "seq", "x_true", "y_raw"])
        while True:
            data, _ = s_x.recvfrom(2048)
            if len(data) != SIZE:
                continue
            ts, x, seq = struct.unpack(FMT, data)
            y = float(x) + random.gauss(0.0, NOISE_STD)
            out = struct.pack(FMT, time.time(), y, seq)
            s_y.sendto(out, (HOST, MITM_PORT))
            w.writerow([time.time(), seq, x, y])
            if seq % 50 == 0:
                f.flush()

if __name__ == "__main__":
    main()
