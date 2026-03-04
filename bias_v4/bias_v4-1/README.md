## More restrict cyber-physical system.
```
cd /Users/hfh/Desktop/0227
make clean && make
sudo setcap cap_sys_ptrace+ep ./bin/attacker_bias
./scripts/run_experiment.sh --mode baseline --duration-sec 30 --ptrace-compat 0
./scripts/run_experiment.sh --mode bias --duration-sec 30 --ptrace-compat 0
```

```
BIAS_RUN=$(ls -dt logs/*_bias | head -n1)
grep -c "memory tamper" "$BIAS_RUN/controller.log"
grep "mode=resolved\\|summary rounds_done\\|injections" "$BIAS_RUN/attacker.log"
```
