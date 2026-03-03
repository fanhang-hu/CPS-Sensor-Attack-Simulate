This experiment achieves the bias attack by performing **Memory Tampering** at the ```controller```,
- The ```sensor``` normally sends data to the ```controller```.
- The attack achieves bias tampering by modifying the measurement values in the controller's memory.
- Under attack, the attack process will naturally generate abnormal system calls ```ptrace, process_vm_readv, process_vm_writev```.


在Linux控制器上执行：

```
sudo make clean && sudo make
```

## 5. 实验步骤

如果你希望一键跑完整流程（自动拉起controller/sensor，并按模式决定是否攻击），可以直接用：

```bash
./scripts/run_experiment.sh --mode baseline --duration-sec 30
./scripts/run_experiment.sh --mode bias --duration-sec 30
```

说明：
- `baseline`模式不启动攻击进程，用于对照组；
- `bias`模式会在warmup后自动执行`attacker_bias`；
- 每次运行都会在`./logs/<时间戳>_<mode>/`下生成分离日志：`controller.log`、`sensor.log`、`attacker.log`、`run_info.txt`；
- `run_info.txt`会记录本次参数，便于你和NoDrop审计结果对齐分析。
- 元数据文件默认也放在本次`run_dir`下（`cps_controller_meta.txt`），可用`--meta-file`改路径。

## 5.1 终端A：启动控制器（基线）

```bash
./bin/controller 20 0.8 80
```

参数含义：
- `20`: setpoint
- `0.8`: P控制器比例系数`Kp`
- `80`: 控制器收到网络值后等待80ms再计算（给攻击线程可见窗口）

启动后会打印：
- 控制器`pid`
- 目标变量`g_latest_measurement`地址
- 元数据文件路径（默认由`CPS_META_FILE`/脚本决定）

## 5.2 终端B：启动传感器

```bash
./bin/sensor 20 1.5 100
```

参数含义：
- `20`: 基础值
- `1.5`: 小幅波动幅度
- `100`: 发送间隔(ms)

此时控制器输出应主要是`net_value == used_value`（无偏置）。

## 5.3 终端C：发起控制器端内存篡改（bias攻击）

```bash
./scripts/run_bias_attack.sh /tmp/cps_controller_meta.txt 4.0 100 80
```

参数含义：
- `4.0`: 每轮给测量值加的偏置
- `100`: 攻击间隔(ms)
- `80`: 攻击轮次

攻击期间你会看到：
- 攻击进程持续打印`old -> new`内存值变化；
- 控制器开始出现`<-- memory tamper`，且`used_value`明显偏离`net_value`，控制输出`u`发生系统性偏差。

## 6. NoDrop中应观测到的异常系统调用

围绕`attacker_bias`进程（PID可从其启动日志获取），应能看到高频的：
- `ptrace`（attach/detach）
- `process_vm_readv`
- `process_vm_writev`

