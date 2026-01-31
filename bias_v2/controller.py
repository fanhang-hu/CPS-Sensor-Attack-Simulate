#!/usr/bin/env python3
import os, socket, struct, time, csv, hmac, hashlib

# -------------------- Configuration --------------------
TS = float(os.environ.get("TS", "0.02"))                   # controller period
PLANT_PORT = int(os.environ.get("PLANT_PORT", "9002"))     # controller -> plant (UDP)
HOST = "127.0.0.1"

# Controller reads measurements from FIFO
MEAS_IN = os.environ.get("MEAS_IN", "/tmp/cps_bias_v2_bus/y.pipe")   # FIFO path to read
KEY_PATH = os.environ.get("KEY_PATH", "/tmp/cps_bias_v2_bus/key.bin")

# FIFO packet format: <double ts, float y, uint32 seq, 32B mac>
FMT_DATA = "<d f I"
DATA_SIZE = struct.calcsize(FMT_DATA)
FMT_MSG = "<d f I 32s"
MSG_SIZE = struct.calcsize(FMT_MSG)

# Simple PI controller
Kp = float(os.environ.get("Kp", "1.2"))
Ki = float(os.environ.get("Ki", "0.6"))
U_MIN = float(os.environ.get("U_MIN", "-10.0"))
U_MAX = float(os.environ.get("U_MAX", "10.0"))

LOG_DIR = "/tmp/cps_bias_v2_logs"
os.makedirs(LOG_DIR, exist_ok=True)
LOG_PATH = os.path.join(LOG_DIR, "controller.csv")

def load_key():
    with open(KEY_PATH, "rb") as f:
        k = f.read()
    if len(k) < 16:
        raise RuntimeError("KEY_PATH too short")
    return k

def setpoint(t):
    # step sequence: 0 -> 5 -> 0 -> 8 (every 5s)
    period = 5.0
    k = int(t // period) % 4
    return [0.0, 5.0, 0.0, 8.0][k]

def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v

def main():
    key = load_key()

    # FIFO open for reading (blocks until writer opens it)
    fd = os.open(MEAS_IN, os.O_RDONLY | os.O_NONBLOCK)

    # UDP for control output
    s_u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    integ = 0.0
    seq_out = 0
    t0 = time.monotonic()
    next_t = t0

    # message stream buffer (FIFO can split reads)
    buf = b""
    last_y = 0.0
    last_seq_in = -1
    last_mac_ok = 1

    with open(LOG_PATH, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t", "seq_out", "seq_in", "r", "y", "mac_ok", "e", "u", "fifo_path"])
        while True:
            now = time.monotonic()
            if now < next_t:
                time.sleep(next_t - now)
                continue

            # Drain FIFO to get latest complete message(s)
            try:
                while True:
                    chunk = os.read(fd, 4096)
                    if not chunk:
                        break
                    buf += chunk
            except BlockingIOError:
                pass

            # Parse all complete messages; keep the latest one
            while len(buf) >= MSG_SIZE:
                one = buf[:MSG_SIZE]
                buf = buf[MSG_SIZE:]
                ts_in, y, seq_in, mac = struct.unpack(FMT_MSG, one)
                data_bytes = struct.pack(FMT_DATA, ts_in, float(y), int(seq_in))
                mac2 = hmac.new(key, data_bytes, hashlib.sha256).digest()
                mac_ok = 1 if hmac.compare_digest(mac, mac2) else 0

                last_y = float(y)
                last_seq_in = int(seq_in)
                last_mac_ok = mac_ok

            # Control update
            t_sim = now - t0
            r = setpoint(t_sim)
            e = r - last_y
            integ += e * TS

            u = Kp * e + Ki * integ
            u = clamp(u, U_MIN, U_MAX)

            # send u to plant
            pkt = struct.pack(FMT_DATA, time.time(), float(u), int(seq_out))
            s_u.sendto(pkt, (HOST, PLANT_PORT))

            w.writerow([time.time(), seq_out, last_seq_in, r, last_y, last_mac_ok, e, u, MEAS_IN])
            if seq_out % 50 == 0:
                f.flush()

            seq_out += 1
            next_t += TS

if __name__ == "__main__":
    main()
