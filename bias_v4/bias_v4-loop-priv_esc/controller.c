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
    int attack_window_ms = 80;
    const char *meta_file_path = get_meta_file_path();
    int use_actuator = env_is_true("CPS_ACTUATOR_ENABLE");

    if (argc > 1) {
        setpoint = atof(argv[1]);
    }
    if (argc > 2) {
        kp = atof(argv[2]);
    }
    if (argc > 3) {
        attack_window_ms = atoi(argv[3]);
        if (attack_window_ms < 0) {
            fprintf(stderr, "attack_window_ms must be >= 0\n");
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
    printf("[controller] listening UDP on 0.0.0.0:%d setpoint=%.3f kp=%.3f window_ms=%d\n",
           SENSOR_PORT, setpoint, kp, attack_window_ms);

    char buf[256];
    while (!g_stop) {
        ssize_t r = recvfrom(sockfd, buf, sizeof(buf) - 1, 0, NULL, NULL);
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

        uint64_t seq = (uint64_t)seq_ull;

        /*
         * Store fresh sensor value first; attacker tampers this value in memory
         * before control law consumes it.
         */
        g_latest_measurement = incoming;

        if (attack_window_ms > 0) {
            usleep((useconds_t)attack_window_ms * 1000U);
        }

        double used = g_latest_measurement;
        double u = kp * (setpoint - used);
        g_snapshot.seq = seq;
        g_snapshot.net_value = incoming;
        g_snapshot.used_value = used;
        g_snapshot.control_output = u;

        if (use_actuator) {
            char out[128];
            int n = snprintf(out, sizeof(out), "%llu %.6f", seq_ull, u);
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

        double diff = used - incoming;
        if (fabs(diff) > 1e-9) {
            printf("[controller] seq=%llu net=%.6f used=%.6f bias=%.6f u=%.6f  <-- memory tamper\n",
                   seq_ull, incoming, used, diff, u);
        } else {
            printf("[controller] seq=%llu net=%.6f used=%.6f u=%.6f\n",
                   seq_ull, incoming, used, u);
        }
        fflush(stdout);
    }

    if (actuator_sock >= 0) {
        close(actuator_sock);
    }
    close(sockfd);
    return 0;
}
