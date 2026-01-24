#!/usr/bin/env python3
import os, socket, struct, time, csv, json

# --- Configuration ---
TS = float(os.environ.get("TS", "0.02"))             # match plant
CTRL_PORT = int(os.environ.get("CTRL_PORT", "9003")) # receive y_spoofed from MITM
PLANT_PORT = int(os.environ.get("PLANT_PORT", "9002"))
HOST = "127.0.0.1"

# Controller gains are periodically reloaded from a file (enables file-tamper attack)
GAIN_PATH = "/tmp/cps_bench_gains.json"
GAIN_RELOAD_SEC = 1.0

FMT = "<d f I"
SIZE = struct.calcsize(FMT)

LOG_DIR = "/tmp/cps_bench_logs"
os.makedirs(LOG_DIR, exist_ok=True)
LOG_PATH = os.path.join(LOG_DIR, "controller.csv")

def load_gains():
    # Default gains if file missing/bad
    gains = {"Kp": 1.2, "Ki": 0.6, "u_min": -10.0, "u_max": 10.0}
    try:
        with open(GAIN_PATH, "r") as f:
            g = json.load(f)
            gains.update({k: float(v) for k, v in g.items() if k in gains})
    except Exception:
        pass
    return gains

def setpoint(t):
    # simple piecewise step every 5 seconds: 0 -> 5 -> 0 -> 8 -> ...
    period = 5.0
    k = int(t // period) % 4
    return [0.0, 5.0, 0.0, 8.0][k]

def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v

def main():
    s_y = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s_y.bind((HOST, CTRL_PORT))
    s_y.setblocking(False)

    s_u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    gains = load_gains()
    last_reload = time.monotonic()

    integ = 0.0
    seq = 0
    t0 = time.monotonic()
    next_t = t0

    last_y = 0.0

    with open(LOG_PATH, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t", "seq", "r", "y", "e", "u", "Kp", "Ki"])
        while True:
            now = time.monotonic()
            if now < next_t:
                time.sleep(next_t - now)
                continue

            # Periodically reload gains (file read syscalls)
            if now - last_reload >= GAIN_RELOAD_SEC:
                gains = load_gains()
                last_reload = now

            # Get latest measurement (non-blocking; hold last value if none)
            try:
                data, _ = s_y.recvfrom(2048)
                if len(data) == SIZE:
                    _, y, _ = struct.unpack(FMT, data)
                    last_y = float(y)
            except BlockingIOError:
                pass

            t_sim = now - t0
            r = setpoint(t_sim)
            e = r - last_y
            integ += e * TS

            u = gains["Kp"] * e + gains["Ki"] * integ
            u = clamp(u, gains["u_min"], gains["u_max"])

            pkt = struct.pack(FMT, time.time(), float(u), seq)
            s_u.sendto(pkt, (HOST, PLANT_PORT))

            w.writerow([time.time(), seq, r, last_y, e, u, gains["Kp"], gains["Ki"]])
            if seq % 50 == 0:
                f.flush()

            seq += 1
            next_t += TS

if __name__ == "__main__":
    main()
