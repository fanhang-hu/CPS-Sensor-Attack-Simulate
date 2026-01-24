#!/usr/bin/env python3
import csv, os
import matplotlib.pyplot as plt

LOG_DIR = "/tmp/cps_bench_logs"
plant = os.path.join(LOG_DIR, "plant.csv")
ctrl  = os.path.join(LOG_DIR, "controller.csv")
mitm  = os.path.join(LOG_DIR, "mitm.csv")

def read_csv(path, cols):
    xs = {c: [] for c in cols}
    if not os.path.exists(path):
        return xs
    with open(path, "r") as f:
        r = csv.DictReader(f)
        for row in r:
            for c in cols:
                xs[c].append(float(row[c]))
    return xs

p = read_csv(plant, ["t","x","u"])
c = read_csv(ctrl,  ["t","r","y","u","e"])
m = read_csv(mitm,  ["t","in_val","out_val"])

plt.figure()
if c["t"]:
    plt.plot(c["t"], c["r"], label="r (setpoint)")
    plt.plot(c["t"], c["y"], label="y (measured)")
plt.xlabel("t (epoch sec)")
plt.ylabel("value")
plt.title("Controller: setpoint vs measurement")
plt.legend()
plt.show()

plt.figure()
if p["t"]:
    plt.plot(p["t"], p["x"], label="x (state)")
if c["t"]:
    plt.plot(c["t"], c["u"], label="u (control)")
plt.xlabel("t (epoch sec)")
plt.ylabel("value")
plt.title("Plant state and control input")
plt.legend()
plt.show()

plt.figure()
if m["t"]:
    plt.plot(m["t"], m["in_val"], label="MITM in")
    plt.plot(m["t"], m["out_val"], label="MITM out")
plt.xlabel("t (epoch sec)")
plt.ylabel("measurement")
plt.title("MITM effect")
plt.legend()
plt.show()
