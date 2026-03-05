## loop

```
sudo setcap cap_sys_ptrace+ep ./bin/attacker_bias
getcap ./bin/attacker_bias

./scripts/run_experiment.sh --mode baseline --duration-sec 30 --ptrace-compat 0 --scenario wheel
./scripts/run_experiment.sh --mode bias --duration-sec 30 --ptrace-compat 0 --scenario wheel
```

**Check ```attacker.log``` and ```controller.log```, you can see the attack worked.**

- ```cd logs/20260304_200514_wheel_bias && vim controller.log``` and ```cd logs/20260304_202824_wheel_bias && vim controller.log```, you can see several attack patches like ```<-- memory tamper```.
While ```20260304_201826_wheel_bias ``` can't see.
