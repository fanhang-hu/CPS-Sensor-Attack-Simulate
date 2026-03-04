## More restrict cyber-physical system.
- 不再明文泄露目标地址: ```controller```写元数据时去掉了```addr_g_latest_measurement```, 只保留就绪信息;控制台输出也不再打印该变量地址.
- 攻击端改为自己算地址: ```attacker_bias```新增运行时地址解析: 读```/proc/<pid>/exe、/proc/<pid>/maps```, 再解析 ELF 符号```g_latest_measurement```得到真实地址; 保留了旧参数模式兼容, 但默认走新模式(mode = resolved).
- 攻击目标发现更灵活: ```attacker_bias```支持```pid|auto|进程名```三种目标输入; ```run_bias_attack.sh```不再依赖元文件里的地址, 默认传```target=auto```并可按进程名找目标.
- ```controller```默认不主动放开```ptrace```, 只在```CPS_PTRACE_COMPAT=1```时才启用兼容放宽; ```run_experiment.sh```加了```--ptrace-compat auto|0|1```, auto会先尝试真实前提(如cap_sys_ptrace), 不满足才自动启用兼容模式，避免攻击失败.

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
