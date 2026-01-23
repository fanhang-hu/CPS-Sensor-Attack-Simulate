import struct
import time

with open('/tmp/sensor_raw', 'wb') as f:
    val = 0.0
    while True:
        f.write(struct.pack('f', val))
        f.flush()
        print(f"[Sensor] Send the original data: {val}")
        val += 1.0
        time.sleep(1)
