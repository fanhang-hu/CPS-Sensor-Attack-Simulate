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
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int signo) {
    (void)signo;
    g_stop = 1;
}

int main(int argc, char **argv) {
    double base = 20.0;
    double amplitude = 1.5;
    int interval_ms = 100;

    if (argc > 1) {
        base = atof(argv[1]);
    }
    if (argc > 2) {
        amplitude = atof(argv[2]);
    }
    if (argc > 3) {
        interval_ms = atoi(argv[3]);
        if (interval_ms <= 0) {
            fprintf(stderr, "interval_ms must be > 0\n");
            return 1;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(SENSOR_PORT);
    if (inet_pton(AF_INET, SENSOR_IP, &dst.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed\n");
        close(sockfd);
        return 1;
    }

    uint64_t seq = 0;
    printf("[sensor] sending UDP to %s:%d\n", SENSOR_IP, SENSOR_PORT);
    while (!g_stop) {
        double noise = sin((double)seq / 10.0) * amplitude;
        double value = base + noise;
        char msg[128];
        int n = snprintf(msg, sizeof(msg), "%llu %.6f",
                         (unsigned long long)seq, value);
        if (n < 0 || n >= (int)sizeof(msg)) {
            fprintf(stderr, "snprintf overflow\n");
            break;
        }

        ssize_t sent = sendto(sockfd, msg, (size_t)n, 0,
                              (struct sockaddr *)&dst, sizeof(dst));
        if (sent < 0) {
            perror("sendto");
            break;
        }

        printf("[sensor] seq=%llu value=%.6f\n",
               (unsigned long long)seq, value);
        seq++;
        usleep((useconds_t)interval_ms * 1000U);
    }

    close(sockfd);
    return 0;
}
