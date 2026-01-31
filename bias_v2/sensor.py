#!/usr/bin/env python3
import os, socket, struct, time, csv, random, hmac, hashlib

# -------------------- Configuration --------------------
SENSOR_PORT = int(os.environ.get("SENSOR_PORT", "9000"))   # plant -> sensor (UDP)
HOST = "127.0.0.1"
NOISE_STD = float(os.environ.get("NOISE_STD", "0.02"))

# Sensor -> Controller channel (FIFO)
MEAS_OUT = os.environ.get("MEAS_OUT", "/tmp/cps_bias_v2_bus/y.pipe")  # FIFO path to write
KEY_PATH = os.environ.get("KEY_PATH", "/tmp/cps_bias_v2_bus/key.bin") # HMAC key

# Packet layout (fixed size) for FIFO:
#   data = <double ts, float y, uint32 seq>
#   mac  = HMAC-SHA256(key, data)  (32 bytes)
FMT_DATA = "<d f I"
DATA_SIZE = struct.calcsize(FMT_DATA)
MAC_SIZE = 32
FMT_MSG = "<d f I 32s"
MSG_SIZE = struct.calcsize(FMT_MSG)

LOG_DIR = "/tmp/cps_bias_v2_bench_logs"
os.makedirs(LOG_DIR, exist_ok=True)
LOG_PATH = os.path.join(LOG_DIR, "sensor.csv")

def load_key():
    with open(KEY_PATH, "rb") as f:
        k = f.read()
    if len(k) < 16:
        raise RuntimeError("KEY_PATH too short")
    return k

def main():
    key = load_key()

    # UDP: recv plant state x
    s_x = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s_x.bind((HOST, SENSOR_PORT))

    # FIFO: write measurements
    # NOTE: opening a FIFO for writing blocks until a reader opens it
    fd = os.open(MEAS_OUT, os.O_WRONLY)

    with open(LOG_PATH, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t", "seq", "x_true", "y_raw", "mac_hex", "fifo_path"])
        while True:
            data, _ = s_x.recvfrom(2048)
            if len(data) < DATA_SIZE:  # plant uses the same <d f I> format
                continue
            ts_in, x, seq = struct.unpack(FMT_DATA, data[:DATA_SIZE])

            # sensor measurement
            y = float(x) + random.gauss(0.0, NOISE_STD)

            ts_out = time.time()
            data_bytes = struct.pack(FMT_DATA, ts_out, float(y), int(seq))
            mac = hmac.new(key, data_bytes, hashlib.sha256).digest()

            msg = struct.pack(FMT_MSG, ts_out, float(y), int(seq), mac)
            os.write(fd, msg)  # syscall-visible "write"

            w.writerow([ts_out, seq, float(x), float(y), mac.hex(), MEAS_OUT])
            if int(seq) % 50 == 0:
                f.flush()

if __name__ == "__main__":
    main()
