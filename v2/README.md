Here, the version 2 use ```auditd``` to monitor the attacks.

First of all, I install auditd in my vitual machine Ubuntu 20.04.
```
sudo apt-get update
sudo apt-get install -y auditd audispd-plugins
```

I don't want it to start ```auditd``` automatically upon startup, so I execute the following:
```
sudo systemctl start auditd
sudo systemctl restart auditd
sudo systemctl status auditd --no-pager
```

If you want to start automatically, you need to execute the following:
```
sudo systemctl enable auditd
sudo systemctl restart auditd
sudo systemctl status auditd --no-pager
```
After that, you can see ```active (running)```.

Now I start test:
```
cd cps_bench

# obtain the absolute path of mitm
MITM_EXE=$(readlink -f ./mitm) 
echo "$MITM_EXE"

# verify that it is indeed an ELF executable file
file "$MITM_EXE"

# clear the current "Running" temporary rules
sudo auditctl -D

# improve the audit buffer
sudo auditctl -b 8192
sudo auditctl -f 1
sudo auditctl -s
```

Create the rule file (write to /etc/audit/rules.d/). Here I met some problems: under my arch, there is no ```poll```, ```rename```, ```unlink```, ```fork``` and ```vfork```, which I use ```sudo ausyscall poll``` to check. Therefore, I modified.
```
sudo bash -c "cat > /etc/audit/rules.d/cps_mitm.rules" <<'RULES'

-a always,exit -F arch=b64 -S recvfrom -S sendto -S recvmsg -S sendmsg -S bind -S connect -F exe=__MITM_EXE__ -k cps_mitm_net

# -a always,exit -F arch=b64 -S ppoll -S poll -F exe=__MITM_EXE__ -k cps_mitm_time
-a always,exit -F arch=b64 -S ppoll -F exe=__MITM_EXE__ -k cps_mitm_time

# -a always,exit -F arch=b64 -S openat -S read -S write -S fsync -S rename -S renameat -S renameat2 -S unlink -S unlinkat -F exe=__MITM_EXE__ -k cps_mitm_file
-a always,exit -F arch=b64 -S openat -S read -S write -S fsync -S renameat -S renameat2 -S unlinkat -F exe=__MITM_EXE__ -k cps_mitm_file

# -a always,exit -F arch=b64 -S execve -S execveat -S clone -S fork -S vfork -F exe=__MITM_EXE__ -k cps_mitm_proc
-a always,exit -F arch=b64 -S execve -S execveat -S clone -F exe=__MITM_EXE__ -k cps_mitm_proc

RULES
```

Now replace the placeholders in the rule file with the **real MITM_EXE path**:
```
sudo sed -i "s|__MITM_EXE__|$MITM_EXE|g" /etc/audit/rules.d/cps_mitm.rules
```

Check the content of the rule file:
```
sudo cat /etc/audit/rules.d/cps_mitm.rules
```

Load the **rules.d** rule:
```
sudo augenrules --load
sudo systemctl restart auditd
```

The verification rules have been loaded into the kernel
```
sudo auditctl -l | grep cps_mitm
```
There are four rules (net/time/file/proc), each stripe ```key=cps_mitm_*```.

Now, I can use ```auditd``` to check my attacks. The examples are as follows:
```
cd cps_bench
mkdir -p audit_out
```

First run benchmark ```pass```. Use ```timestamp``` to define the time domine.
```
START="$(date '%H:%M:%S')"
echo "START=$START"
```

Run benchmark:
```
./run_demo.sh pass
```

Then, pitch the end.
```
END="$(date '+%Y-%m-%d %H:%M:%S')"
echo "END=$END"
```

Finally, export four types of events and check.
```
sudo ausearch -k cps_mitm_net  --start "$START" --end "$END" -i > audit_out/pass.net.txt
sudo ausearch -k cps_mitm_time --start "$START" --end "$END" -i > audit_out/pass.time.txt
sudo ausearch -k cps_mitm_file --start "$START" --end "$END" -i > audit_out/pass.file.txt
sudo ausearch -k cps_mitm_proc --start "$START" --end "$END" -i > audit_out/pass.proc.txt
wc -l audit_out/pass.net.txt audit_out/pass.time.txt
```
