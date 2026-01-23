import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/sensor_raw'

print(f"[App] is monitoring: {path}")
with open(path, 'rb') as f:
    while True:
        data = f.read(4)
        if data:
            val = struct.unpack('f', data)[0]
            print(f"[App] receive: {val}")
