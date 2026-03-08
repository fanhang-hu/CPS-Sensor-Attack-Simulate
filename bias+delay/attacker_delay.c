#include "common.h"

#ifndef __linux__
#include <stdio.h>
int main(void) {
    fprintf(stderr, "attacker_delay only supports Linux.\n");
    return 1;
}
#else

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__NR_pidfd_open)
#define SYS_PIDFD_OPEN __NR_pidfd_open
#elif defined(SYS_pidfd_open)
#define SYS_PIDFD_OPEN SYS_pidfd_open
#endif

#if defined(__NR_pidfd_send_signal)
#define SYS_PIDFD_SEND_SIGNAL __NR_pidfd_send_signal
#elif defined(SYS_pidfd_send_signal)
#define SYS_PIDFD_SEND_SIGNAL SYS_pidfd_send_signal
#endif

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int signo) {
    (void)signo;
    g_stop = 1;
}

static int is_number(const char *s) {
    if (!s || *s == '\0') {
        return 0;
    }
    if (*s == '+' || *s == '-') {
        s++;
    }
    if (*s == '\0') {
        return 0;
    }
    while (*s) {
        if (!isdigit((unsigned char)*s)) {
            return 0;
        }
        s++;
    }
    return 1;
}

static pid_t find_pid_by_comm(const char *proc_name) {
    DIR *dir = opendir("/proc");
    if (!dir) {
        return -1;
    }

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)ent->d_name[0])) {
            continue;
        }

        pid_t pid = (pid_t)atoi(ent->d_name);
        if (pid <= 0) {
            continue;
        }

        char comm_path[128];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
        FILE *fp = fopen(comm_path, "r");
        if (!fp) {
            continue;
        }

        char comm[128];
        if (!fgets(comm, sizeof(comm), fp)) {
            fclose(fp);
            continue;
        }
        fclose(fp);

        size_t len = strlen(comm);
        if (len > 0 && comm[len - 1] == '\n') {
            comm[len - 1] = '\0';
        }

        if (strcmp(comm, proc_name) == 0) {
            closedir(dir);
            return pid;
        }
    }

    closedir(dir);
    errno = ESRCH;
    return -1;
}

static int pid_alive(pid_t pid) {
    if (pid <= 0) {
        return 0;
    }
    if (kill(pid, 0) == 0) {
        return 1;
    }
    return errno == EPERM;
}

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)ts.tv_nsec / 1000000LL;
}

static int sleep_ms_interruptible(int ms) {
    if (ms <= 0) {
        return 0;
    }

    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;

    while (!g_stop) {
        struct timespec rem = {0, 0};
        if (nanosleep(&req, &rem) == 0) {
            return 0;
        }
        if (errno != EINTR) {
            return -1;
        }
        req = rem;
    }
    return 0;
}

static int jittered_ms(int base_ms, int jitter_ms) {
    if (jitter_ms <= 0) {
        return base_ms;
    }
    int span = 2 * jitter_ms + 1;
    int delta = (rand() % span) - jitter_ms;
    int v = base_ms + delta;
    if (v < 1) {
        v = 1;
    }
    return v;
}

static int open_pidfd(pid_t pid) {
#if defined(SYS_PIDFD_OPEN)
    return (int)syscall(SYS_PIDFD_OPEN, pid, 0U);
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

static int send_signal_pidfd(int pidfd, int sig) {
#if defined(SYS_PIDFD_SEND_SIGNAL)
    return (int)syscall(SYS_PIDFD_SEND_SIGNAL, pidfd, sig, NULL, 0U);
#else
    (void)pidfd;
    (void)sig;
    errno = ENOSYS;
    return -1;
#endif
}

static int send_target_signal(pid_t pid, int pidfd, int sig, const char **method_out) {
    if (pidfd >= 0) {
        if (send_signal_pidfd(pidfd, sig) == 0) {
            if (method_out) {
                *method_out = "pidfd_send_signal";
            }
            return 0;
        }
    }

    if (kill(pid, sig) == 0) {
        if (method_out) {
            *method_out = "kill";
        }
        return 0;
    }

    return -1;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s <pid|auto|proc_name> <hold_ms> [interval_ms] [rounds] [proc_name] [jitter_ms]\n"
            "Example: %s auto 180 250 80 sensor 30\n",
            prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    const char *target_spec = argv[1];
    int hold_ms = atoi(argv[2]);
    int interval_ms = 250;
    int rounds = 80;
    const char *proc_name = "sensor";
    int jitter_ms = 30;

    if (argc > 3) {
        interval_ms = atoi(argv[3]);
    }
    if (argc > 4) {
        rounds = atoi(argv[4]);
    }
    if (argc > 5) {
        proc_name = argv[5];
    }
    if (argc > 6) {
        jitter_ms = atoi(argv[6]);
    }

    if (hold_ms <= 0 || interval_ms <= 0 || rounds <= 0 || jitter_ms < 0) {
        usage(argv[0]);
        return 1;
    }

    pid_t pid = -1;
    if (strcmp(target_spec, "auto") == 0) {
        pid = find_pid_by_comm(proc_name);
    } else if (is_number(target_spec)) {
        pid = (pid_t)strtol(target_spec, NULL, 10);
    } else {
        proc_name = target_spec;
        pid = find_pid_by_comm(proc_name);
    }

    if (pid <= 0) {
        perror("find target pid");
        fprintf(stderr, "Hint: ensure process name '%s' exists.\n", proc_name);
        return 1;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    srand((unsigned int)(time(NULL) ^ getpid()));

    int pidfd = open_pidfd(pid);
    if (pidfd >= 0) {
        printf("[delay_attacker] pidfd_open success pidfd=%d target_pid=%d\n", pidfd, pid);
    } else {
        printf("[delay_attacker] pidfd_open unavailable, fallback to kill(2): errno=%d\n", errno);
    }

    printf("[delay_attacker] target_pid=%d proc_name=%s hold_ms=%d interval_ms=%d rounds=%d jitter_ms=%d\n",
           pid, proc_name, hold_ms, interval_ms, rounds, jitter_ms);

    int rounds_done = 0;
    int pauses = 0;
    int64_t total_hold_ms = 0;
    int paused = 0;

    for (int i = 0; i < rounds && !g_stop; ++i) {
        if (!pid_alive(pid)) {
            fprintf(stderr, "[delay_attacker] target pid %d not alive\n", pid);
            break;
        }

        int hold_this = jittered_ms(hold_ms, jitter_ms);
        int rest_this = jittered_ms(interval_ms, jitter_ms);
        const char *method = "unknown";

        if (send_target_signal(pid, pidfd, SIGSTOP, &method) != 0) {
            perror("send SIGSTOP");
            break;
        }
        paused = 1;
        int64_t t0_ms = now_ms();

        if (sleep_ms_interruptible(hold_this) != 0) {
            perror("nanosleep(hold)");
            break;
        }

        if (send_target_signal(pid, pidfd, SIGCONT, &method) != 0) {
            perror("send SIGCONT");
            break;
        }
        paused = 0;

        int64_t t1_ms = now_ms();
        int64_t actual_hold_ms = (t1_ms >= t0_ms) ? (t1_ms - t0_ms) : (int64_t)hold_this;

        rounds_done++;
        pauses++;
        total_hold_ms += actual_hold_ms;

        printf("[delay_attacker] round=%d method=%s hold_req_ms=%d hold_actual_ms=%lld rest_ms=%d\n",
               i, method, hold_this, (long long)actual_hold_ms, rest_this);
        fflush(stdout);

        if (sleep_ms_interruptible(rest_this) != 0) {
            perror("nanosleep(rest)");
            break;
        }
    }

    if (paused) {
        const char *method = "unknown";
        if (send_target_signal(pid, pidfd, SIGCONT, &method) == 0) {
            printf("[delay_attacker] emergency resume sent via %s\n", method);
        } else {
            perror("send emergency SIGCONT");
        }
    }

    if (pidfd >= 0) {
        close(pidfd);
    }

    printf("[delay_attacker] summary rounds_done=%d pauses=%d total_hold_ms=%lld\n",
           rounds_done, pauses, (long long)total_hold_ms);
    printf("[delay_attacker] finished.\n");
    return 0;
}

#endif
