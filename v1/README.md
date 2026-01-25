Here is a new version of cps sensor attack simulation, which create more attacks and make it more complete. Importantly, this version is **closed-loop Control**.

We create six attacks: Bias, Delay, Replay, Jitter, Drop, Scale.

Here are some operates and syscalls, which can be caught by [<u>NoDrop</u>]
| item | syscall |
|:--------|:--------:|
| network   | socket,bind,recvfrom,sendto,poll|
| file      | openat,read,write,rename,fsync  |
| process   | fork,clone,execve               |
