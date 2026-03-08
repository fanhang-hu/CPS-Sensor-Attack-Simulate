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

Here are 4 logs under /home/hfh/bias+delay/logs/: ```20260308_155149_wheel_baseline, 20260308_155450_wheel_fullchain, 20260308_162755_wheel_delay, 20260308_183405_wheel_delay```. ```20260308_183405_wheel_delay``` was under the monitor of **NoDrop**.

```
syssecure@312:/home/hfh/bias+delay/logs$ cd 20260308_183405_wheel_delay
syssecure@312:/home/hfh/bias+delay/logs/20260308_183405_wheel_delay$ grep "summary rounds_done" attacker.log
[delay_attacker] summary rounds_done=72 pauses=72 total_hold_ms=13003
syssecure@312:/home/hfh/bias+delay/logs/20260308_183405_wheel_delay$ awk -F'delta_us=' '/delta_us=/{split($2,a," "); if(a[1]>m)m=a[1]; if(a[1]>=180000)c++} END{print "max=",m," ge180k=",c}' controller.log
max= 264271  ge180k= 367
syssecure@312:/home/hfh/bias+delay/logs/20260308_183405_wheel_delay$ cd ../*_*_wheel_baseline
syssecure@312:/home/hfh/bias+delay/logs/20260308_155149_wheel_baseline$ awk -F'delta_us=' '/delta_us=/{split($2,a," "); if(a[1]>m)m=a[1]; if(a[1]>=180000)c++} END{print "max=",m," ge180k=",c}' controller.log
max= 99063  ge180k=
```

We can see that the ```controller.log``` is different.
