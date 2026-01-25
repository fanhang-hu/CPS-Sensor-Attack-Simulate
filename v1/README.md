Here is a new version of cps sensor attack simulation, which create more attacks and make it more complete. Importantly, this version is **closed-loop control**.

We create six attacks: Bias, Delay, Replay, Jitter, Drop, Scale.

Here are some operates and syscalls, which can be caught by [<u>NoDrop</u>](https://www.usenix.org/conference/usenixsecurity23/presentation/jiang-peng) and detect by [<u>NodLink</u>](https://dx.doi.org/10.14722/ndss.2024.23204).

| item | syscall |
|:--------|:--------:|
| network   | socket,bind,recvfrom,sendto,poll|
| file      | openat,read,write,rename,fsync  |
| process   | fork,clone,execve               |

Now, I'd like to analyze the closed-loop control and the results. **plant.py** is the work scene we assumed. It's a 1D plant. **sensor.py** will sent specific signals to **controller.py**. **mitm.c** is the attack, which may tamper the signal by Bias, Delay, Replay, Jitter, Drop and Scale. **run_demo.sh** integrate plant.py, sensor.py, controller.py and mitm.c as a script. **plot.py** draw three pictures to illustrate the demo. **parse_seq.py** try to analyze Replay attack.

**Figure_1.png** shows that controller can catch signals immediately. **Figure_2.png** shows that attacks can affact the plant. **Figure_3.png** shows the Bias attack (+1.5).

