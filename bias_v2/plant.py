#!/usr/bin/env python3
import os, socket, struct, time, csv

# --- Configuration ---
TS = float(os.environ.get("TS", "0.02"))           # 20ms (50Hz)
PLANT_PORT = int(os.environ.get("PLANT_PORT", "9002"))
SENSOR_PORT = int(os.environ.get("SENSOR_PORT", "9000"))
HOST = "127.0.0.1"

# Simple stable 1D plant: x[k+1] = a*x + b*u
A = float(os.environ.get("A", "0.98"))
B = float(os.environ.get("B", "0.05"))
U_MIN, U_MAX = -10.0, 10.0

# Binary message: <double ts, float value, uint32 seq>
FMT = "<d f I"
SIZE = struct.calcsize(FMT)

LOG_DIR = "/tmp/cps_bias_v2_logs"
os.makedirs(LOG_DIR, exist_ok=True)
LOG_PATH = os.path.join(LOG_DIR, "plant.csv")

def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v

def main():
    # recv u (controller -> plant)
    s_u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s_u.bind((HOST, PLANT_PORT))
    s_u.setblocking(False)

    # send x (plant -> sensor)
    s_x = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    x = 0.0
    seq = 0
    u_last = 0.0

    t0 = time.monotonic()
    next_t = t0

    with open(LOG_PATH, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t", "seq", "x", "u"])
        while True:
            now = time.monotonic()
            if now < next_t:
                time.sleep(next_t - now)
                continue
            # Try receive latest u (non-blocking)
            try:
                data, _ = s_u.recvfrom(SIZE)
                if len(data) == SIZE:
                    _, u, _ = struct.unpack(FMT, data)
                    u_last = clamp(u, U_MIN, U_MAX)
            except BlockingIOError:
                pass

            # Plant update
            x = A * x + B * u_last

            ts = time.time()
            pkt = struct.pack(FMT, ts, float(x), seq)
            s_x.sendto(pkt, (HOST, SENSOR_PORT))

            w.writerow([ts, seq, x, u_last])
            if seq % 50 == 0:
                f.flush()

            seq += 1
            next_t += TS

if __name__ == "__main__":
    main()
