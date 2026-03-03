#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int dump_meta_file(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror("fopen meta file");
        return -1;
    }

    fprintf(fp, "pid=%d\n", getpid());
    fprintf(fp, "addr_g_latest_measurement=%p\n", (void *)&g_latest_measurement);
    fprintf(fp, "sensor_port=%d\n", SENSOR_PORT);
    fclose(fp);
    return 0;
}

int main(int argc, char **argv) {
    double setpoint = 20.0;
    double kp = 0.8;
    int attack_window_ms = 80;
    const char *meta_file_path = get_meta_file_path();

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
    if (prctl(PR_SET_PTRACER, (unsigned long)PR_SET_PTRACER_ANY, 0L, 0L, 0L) != 0) {
        perror("prctl(PR_SET_PTRACER)");
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

    if (dump_meta_file(meta_file_path) == 0) {
        printf("[controller] metadata written to %s\n", meta_file_path);
    }
    printf("[controller] pid=%d addr(g_latest_measurement)=%p\n",
           getpid(), (void *)&g_latest_measurement);
    printf("[controller] listening UDP on 0.0.0.0:%d setpoint=%.3f kp=%.3f\n",
           SENSOR_PORT, setpoint, kp);

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

    close(sockfd);
    return 0;
}
