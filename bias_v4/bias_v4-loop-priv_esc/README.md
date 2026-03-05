## priv_esc
Terminal A, start maintenance service
```
sudo ./bin/cps_maintd /tmp/cps_maintd.sock
```

Terminal B,
```
sudo insmod ./nodrop.ko
```

Terminal C,
```
./scripts/run_experiment.sh --mode baseline --duration-sec 30 --scenario wheel --ptrace-compat 0
./scripts/run_experiment.sh --mode fullchain --duration-sec 30 --scenario wheel --ptrace-compat 0
```

Terminal B,
```
sudo rmmod nodrop
```

Here are 3 logs in server

- ```20260305_175407_wheel_fullchain``` is a test, **didn't run nodrop**.

- ```20260305_180930_wheel_fullchain``` and ```20260305_183332_wheel_fullchain``` are two tests **ran nodrop**, which stored at ```/tmp/nodrop/0305-bias-v4-loop-priv_esc``` and ```/tmp/nodrop/0305-bias-v4-loop-priv_esc```

**Check ```attacker.log``` and ```controller.log```, you can see the attack worked.**

- ```cd logs/20260305_175407_wheel_fullchain && vim controller.log```, you can see several attack patches like ```<-- memory tamper```.
While ```20260305_180930_wheel_fullchain``` and ```20260305_183332_wheel_fullchain``` can't see.
