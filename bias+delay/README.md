## Integrated delay attack under ```./bias_v4/bias_v4-loop-priv_esc-window```
You can use the following cmd to run this test,

```
sudo make clean && sudo make
./scripts/run_experiment.sh --mode delay --scenario wheel --duration-sec 30 \
  --warmup-sec 2 \
  --delay-target sensor \
  --delay-hold-ms 180 \
  --delay-interval-ms 250 \
  --delay-rounds 80 \
  --delay-jitter-ms 30
```

After that, you can use the following method to check whether the attack is successful or not:
- First of all, ```vim attacker.log``` to find if there are several loops of ```SIGSTOP/SIGCONT```. If it is worked, you can see ```round=... method=... hold_req_ms=... hold_actual_ms=...``` in the log.
- Secondly, ```vim controller.log``` to check ```delta_us``` and ```seq```. For example, maybe ```delta_us``` significantly increased (usually approaching/exceeding the peak of hold_ms), or the same ```seq``` is repeated continuously for a longer time.

You can use the following cmd to check ```controller.log```,
```
grep "summary rounds_done" attacker.log
awk -F'delta_us=' '/delta_us=/{split($2,a," "); if(a[1]>m)m=a[1]; if(a[1]>=180000)c++} END{print "max=",m," ge180k=",c}' controller.log
```

