Here is a new version of cps sensor attack simulation, which create more attacks and make it more complete.

| item | syscall |
|:--------|:--------:|
| network   | socket,bind,recvfrom,sendto,poll|
| file      | openat,read,write,rename,fsync  |
| process   | fork,clone,execve               |
