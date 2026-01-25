Here is a new version of cps sensor attack simulation, which create more attacks and make it more complete. Importantly, this version is **closed-loop control**.

We create six attacks: Bias, Delay, Replay, Jitter, Drop, Scale.

Here are some operates and syscalls, which can be caught by [<u>NoDrop</u>](https://www.usenix.org/conference/usenixsecurity23/presentation/jiang-peng) and detect by [<u>NodLink</u>](https://dx.doi.org/10.14722/ndss.2024.23204).

| item | syscall |
|:--------|:--------:|
| network   | socket,bind,recvfrom,sendto,poll|
| file      | openat,read,write,rename,fsync  |
| process   | fork,clone,execve               |

Now, I'd like to analyze the closed-loop control and the results. ```plant.py``` is the work scene we assumed. It's a 1D plant. ```sensor.py``` will sent specific signals to ```controller.py```. ```mitm.c``` is the attack, which may tamper the signal by Bias, Delay, Replay, Jitter, Drop and Scale. ```run_demo.sh``` integrate plant.py, sensor.py, controller.py and mitm.c as a script. ```plot.py``` draw three pictures to illustrate the demo. ```parse_seq.py``` try to analyze Replay attack.

```Figure_1.png``` shows that controller can catch signals immediately. ```Figure_2.png``` shows that attacks can affact the plant. ```Figure_3.png``` shows the Bias attack (+1.5).

Now I would like to analyze ```/traces```:

In ```pass.25692```, there's four syscalls: recvfrom(receive measurement package), sendto(send measurement package), poll(wait for socket or timeout), write. Compared to ```bias.*, delay.*, replay.*, jitter.*, drop.*, scale.*```, there's no new syscall exist. I would like to claim that delay and jitter attack here, are not use ```nanosleep```, they use ```ppoll(timeout)```.

First of all, I look the count of ```recvfrom``` and ```sendto``` to judge **drop attack**. We can see that ```recvfrom > sendto (1185 vs. 1086)```, which means the drop attack worked.

| attack | recvfrom vs. sendto|
|:--------|:--------:|
| pass    | 1010 vs. 1010 |
| bias    | 1119 vs. 1119 |
| delay   | 1132 vs. 1132 |
| replay  | 975 vs.  975  |
| jitter  | 1188 vs. 1187 |
| drop    | 1185 vs. 1086 |
| scale   | 1186 vs. 1186 |

After that, I look the ```ppoll``` and ```timeout``` to judge **delay attack** and **jitter attack**.

| attack | ppoll | timeout |
|:--------|:--------:|:--------:|
| pass    | 1011 | 0 |
| bias    | 1120 | 0 |
| delay   | 2461 | 1328 |
| replay  | 976  | 0 |
| jitter  | 2899 | 1711 |
| drop    | 1186 | 0 |
| scale   | #    | # |

To analyze **relpay attack**, I need to judge whether the ```seq`` is repeat or rollback from ```sendto payload```. The seq of pass: ```total = 985, non_increasing = 8, adjacent_duplicates = 0```, while the seq of replay: ```total = 975, non_increasing = 460, adjacent_duplicates = 31```. That means **replay attack** is sending old data and shuffling sequences.

We can't clearly see the difference between **bias and pass** and **scale and pass**. But ```Figure_3.png``` shows the **bias attack** worked.
