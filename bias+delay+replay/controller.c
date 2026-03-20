#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

static volatile sig_atomic_t g_stop = 0;

/*
 * Attack target: attacker process tampers this variable in controller memory.
 * Marked volatile so reads/writes always hit memory.
 */
volatile double g_latest_measurement = 0.0;
volatile controller_snapshot_t g_snapshot = {0};

static const char *get_meta_file_path(void) {
    const char *env_path = getenv("CPS_META_FILE");
    if (env_path && env_path[0] != '\0') {
        return env_path;
    }
    return META_FILE;
}

static void on_sigint(int signo) {
    (void)signo;
    g_stop = 1;
}

static long long timespec_delta_us(const struct timespec *start, const struct timespec *end) {
    long long sec = (long long)end->tv_sec - (long long)start->tv_sec;
    long long nsec = (long long)end->tv_nsec - (long long)start->tv_nsec;
    return sec * 1000000LL + nsec / 1000LL;
}

static void timespec_add_ms(struct timespec *ts, int ms) {
    if (ms <= 0) {
        return;
    }
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static int timespec_cmp(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec != b->tv_sec) {
        return (a->tv_sec > b->tv_sec) ? 1 : -1;
    }
    if (a->tv_nsec != b->tv_nsec) {
        return (a->tv_nsec > b->tv_nsec) ? 1 : -1;
    }
    return 0;
}

static int env_is_true(const char *name) {
    const char *val = getenv(name);
    if (!val || val[0] == '\0') {
        return 0;
    }
    return strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 || strcasecmp(val, "yes") == 0;
}

static int dump_meta_file(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror("fopen meta file");
        return -1;
    }

    fprintf(fp, "status=ready\n");
    fprintf(fp, "sensor_port=%d\n", SENSOR_PORT);
    fprintf(fp, "pid=%d\n", getpid());
    fclose(fp);
    return 0;
}

int main(int argc, char **argv) {
    double setpoint = 20.0;
    double kp = 0.8;
    int poll_timeout_ms = 10;
    int control_period_ms = 10;
    const char *meta_file_path = get_meta_file_path();
    int use_actuator = env_is_true("CPS_ACTUATOR_ENABLE");

    if (argc > 1) {
        setpoint = atof(argv[1]);
    }
    if (argc > 2) {
        kp = atof(argv[2]);
    }
    if (argc > 3) {
        poll_timeout_ms = atoi(argv[3]);
        if (poll_timeout_ms < 0) {
            fprintf(stderr, "poll_timeout_ms must be >= 0\n");
            return 1;
        }
    }
    if (argc > 4) {
        control_period_ms = atoi(argv[4]);
        if (control_period_ms <= 0) {
            fprintf(stderr, "control_period_ms must be > 0\n");
            return 1;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

#if defined(__linux__) && defined(PR_SET_DUMPABLE)
    if (prctl(PR_SET_DUMPABLE, 1L, 0L, 0L, 0L) != 0) {
        perror("prctl(PR_SET_DUMPABLE)");
    }
#endif
#if defined(__linux__) && defined(PR_SET_PTRACER) && defined(PR_SET_PTRACER_ANY)
    /*
     * Realistic default: do not globally relax ptrace policy.
     * Enable compatibility mode only when explicitly requested in lab.
     */
    const char *compat = getenv("CPS_PTRACE_COMPAT");
    if (compat && strcmp(compat, "1") == 0) {
        if (prctl(PR_SET_PTRACER, (unsigned long)PR_SET_PTRACER_ANY, 0L, 0L, 0L) != 0) {
            perror("prctl(PR_SET_PTRACER)");
        }
    }
#endif

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(sockfd);
        return 1;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(SENSOR_PORT);
    if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }

    int actuator_sock = -1;
    struct sockaddr_in actuator_dst;
    memset(&actuator_dst, 0, sizeof(actuator_dst));
    if (use_actuator) {
        actuator_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (actuator_sock < 0) {
            perror("socket(actuator_sock)");
            close(sockfd);
            return 1;
        }
        actuator_dst.sin_family = AF_INET;
        actuator_dst.sin_port = htons(ACTUATOR_PORT);
        if (inet_pton(AF_INET, SENSOR_IP, &actuator_dst.sin_addr) != 1) {
            fprintf(stderr, "inet_pton failed\n");
            close(actuator_sock);
            close(sockfd);
            return 1;
        }
    }

    if (dump_meta_file(meta_file_path) == 0) {
        printf("[controller] metadata written to %s\n", meta_file_path);
    }
    printf("[controller] pid=%d\n", getpid());
    if (use_actuator) {
        printf("[controller] mode=closed-loop actuator=%s:%d\n", SENSOR_IP, ACTUATOR_PORT);
    } else {
        printf("[controller] mode=standalone (no actuator output)\n");
    }
    printf("[controller] listening UDP on 0.0.0.0:%d setpoint=%.3f kp=%.3f poll_timeout_ms=%d control_period_ms=%d\n",
           SENSOR_PORT, setpoint, kp, poll_timeout_ms, control_period_ms);

    char buf[256];
    unsigned long long total_samples = 0;
    unsigned long long tamper_samples = 0;
    int has_sample = 0;
    uint64_t latest_seq = 0;
    double latest_incoming = 0.0;
    struct timespec latest_ingest_ts = {0, 0};
    struct timespec next_control_ts = {0, 0};
    (void)clock_gettime(CLOCK_MONOTONIC, &next_control_ts);
    timespec_add_ms(&next_control_ts, control_period_ms);

    while (!g_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);

        struct timeval tv;
        tv.tv_sec = poll_timeout_ms / 1000;
        tv.tv_usec = (poll_timeout_ms % 1000) * 1000;

        int sel = select(sockfd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }
        if (sel > 0 && FD_ISSET(sockfd, &rfds)) {
            struct sockaddr_storage src_addr;
            socklen_t src_len = sizeof(src_addr);
            ssize_t r = recvfrom(sockfd, buf, sizeof(buf) - 1, 0,
                                 (struct sockaddr *)&src_addr, &src_len);
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("recvfrom");
                break;
            }
            buf[r] = '\0';

            unsigned long long seq_ull = 0;
            double incoming = 0.0;
            if (sscanf(buf, "%llu %lf", &seq_ull, &incoming) != 2) {
                fprintf(stderr, "[controller] parse error: %s\n", buf);
                continue;
            }

            latest_seq = (uint64_t)seq_ull;
            latest_incoming = incoming;
            /*
             * Store fresh sensor value; control law consumes on periodic scan tick.
             * This models realistic CPS controller scheduling instead of blocking sleep.
             */
            (void)clock_gettime(CLOCK_MONOTONIC, &latest_ingest_ts);
            g_latest_measurement = incoming;
            has_sample = 1;
        }

        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        if (has_sample && timespec_cmp(&now, &next_control_ts) >= 0) {
            double used = g_latest_measurement;
            long long delta_us = timespec_delta_us(&latest_ingest_ts, &now);
            double u = kp * (setpoint - used);
            g_snapshot.seq = latest_seq;
            g_snapshot.net_value = latest_incoming;
            g_snapshot.used_value = used;
            g_snapshot.control_output = u;

            if (use_actuator) {
                char out[128];
                int n = snprintf(out, sizeof(out), "%llu %.6f",
                                 (unsigned long long)latest_seq, u);
                if (n < 0 || n >= (int)sizeof(out)) {
                    fprintf(stderr, "[controller] snprintf overflow\n");
                    break;
                }
                if (sendto(actuator_sock, out, (size_t)n, 0,
                           (struct sockaddr *)&actuator_dst, sizeof(actuator_dst)) < 0) {
                    perror("sendto actuator");
                    break;
                }
            }

            double diff = used - latest_incoming;
            total_samples++;
            if (fabs(diff) > 1e-9) {
                tamper_samples++;
                printf("[controller] seq=%llu net=%.6f used=%.6f bias=%.6f u=%.6f delta_us=%lld  <-- memory tamper\n",
                       (unsigned long long)latest_seq, latest_incoming, used, diff, u, delta_us);
            } else {
                printf("[controller] seq=%llu net=%.6f used=%.6f u=%.6f delta_us=%lld\n",
                       (unsigned long long)latest_seq, latest_incoming, used, u, delta_us);
            }
            fflush(stdout);

            next_control_ts = now;
            timespec_add_ms(&next_control_ts, control_period_ms);
        }
    }

    printf("[controller] summary total=%llu tamper=%llu tamper_ratio=%.4f\n",
           total_samples, tamper_samples,
           total_samples ? (double)tamper_samples / (double)total_samples : 0.0);

    if (actuator_sock >= 0) {
        close(actuator_sock);
    }
    close(sockfd);
    return 0;
}
