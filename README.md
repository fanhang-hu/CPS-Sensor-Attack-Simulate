# CPS-Sensor-Attack-Simulat

I conducted a simulation of sensor attacks on the host, and used **strace** to track whether abnormal syscall occurred.

Before that, I tried [<u>CPSim</u>](https://sim.cpsec.org/), a Toolbox includes a CPS simulator and a set of security tools. This Toolbox contains bias, delay and replay attacks on LTI model (e.g. vehicle turning, DC motor speed, etc) and several nonlinear models. What's more, I also tried [<u>ArduPilot SITL</u>](https://ardupilot.org/), a high-fidelity simulation. But I still remain skeptical about whether sensor attacks can be monitored by system-level calls. Therefore, I create a demo to check whether sensor attack simulation can be monitored.

sensor_sim.py simulates a sensor that generates increasing data every second and transfer to pip [<u>sensor_raw</u>].

app.py represents applications that originally intended to use sensor data (e.g. autonomous driving assistance, dashboards, etc).

attack.py simulates bias and delay attacks.

Now, you can run and monitor the syscalls.

First of all, you should ```gcc attack.c -o attack```. And then,

```
python3 sensor_sim.py

python3 app.py /tmp/sensor_spoofed

strace -T -o attack_trace.txt -e trace=read,write,nanosleep,clock_nanosleep ./attack
```

Then, you will get attack_trace.txt and you can analysis it.

Compared ```read(3, "\0\0\0\0", 4)``` and ```write(4, "\0\0 A", 4)```, ```read(3, "\0\0\200?", 4)``` and ```write(4, "\0\0000A", 4)```, etc, the bias attack successed.

```clock_nanosleep(CLOCK_REALTIME, 0, {tv_sec=0, tv_nsec=500000000}, NULL) = 0 <0.505256>``` means the delay attack successed.
