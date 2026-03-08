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

static double clamp(double v, double lo, double hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

int main(int argc, char **argv) {
    double max_torque = 12.0;
    double alpha = 0.65;

    if (argc > 1) {
        max_torque = atof(argv[1]);
        if (max_torque <= 0.0) {
            fprintf(stderr, "max_torque must be > 0\n");
            return 1;
        }
    }
    if (argc > 2) {
        alpha = atof(argv[2]);
        if (alpha < 0.0 || alpha > 1.0) {
            fprintf(stderr, "alpha must be in [0,1]\n");
            return 1;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int recv_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_sock < 0) {
        perror("socket(recv)");
        return 1;
    }

    int reuse = 1;
    if (setsockopt(recv_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(recv_sock);
        return 1;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(ACTUATOR_PORT);
    if (bind(recv_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        close(recv_sock);
        return 1;
    }

    int send_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_sock < 0) {
        perror("socket(send)");
        close(recv_sock);
        return 1;
    }

    struct sockaddr_in plant_dst;
    memset(&plant_dst, 0, sizeof(plant_dst));
    plant_dst.sin_family = AF_INET;
    plant_dst.sin_port = htons(PLANT_TORQUE_PORT);
    if (inet_pton(AF_INET, SENSOR_IP, &plant_dst.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed\n");
        close(send_sock);
        close(recv_sock);
        return 1;
    }

    double applied_torque = 0.0;
    char buf[256];

    printf("[actuator] listen 0.0.0.0:%d -> plant %s:%d max_torque=%.3f alpha=%.3f\n",
           ACTUATOR_PORT, SENSOR_IP, PLANT_TORQUE_PORT, max_torque, alpha);

    while (!g_stop) {
        struct sockaddr_storage src_addr;
        socklen_t src_len = sizeof(src_addr);
        ssize_t r = recvfrom(recv_sock, buf, sizeof(buf) - 1, 0,
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
        double cmd = 0.0;
        if (sscanf(buf, "%llu %lf", &seq_ull, &cmd) != 2) {
            fprintf(stderr, "[actuator] parse error: %s\n", buf);
            continue;
        }

        double sat_cmd = clamp(cmd, -max_torque, max_torque);
        applied_torque = alpha * applied_torque + (1.0 - alpha) * sat_cmd;

        char out[128];
        int n = snprintf(out, sizeof(out), "%llu %.6f", seq_ull, applied_torque);
        if (n < 0 || n >= (int)sizeof(out)) {
            fprintf(stderr, "snprintf overflow\n");
            break;
        }

        if (sendto(send_sock, out, (size_t)n, 0,
                   (struct sockaddr *)&plant_dst, sizeof(plant_dst)) < 0) {
            perror("sendto");
            break;
        }

        printf("[actuator] seq=%llu cmd=%.6f sat=%.6f applied=%.6f\n",
               seq_ull, cmd, sat_cmd, applied_torque);
        fflush(stdout);
    }

    close(send_sock);
    close(recv_sock);
    return 0;
}
