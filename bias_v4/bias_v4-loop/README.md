## loop

```
sudo setcap cap_sys_ptrace+ep ./bin/attacker_bias
getcap ./bin/attacker_bias

./scripts/run_experiment.sh --mode baseline --duration-sec 30 --ptrace-compat 0 --scenario wheel
./scripts/run_experiment.sh --mode bias --duration-sec 30 --ptrace-compat 0 --scenario wheel

```
