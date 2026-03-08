#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int signo) {
    (void)signo;
    g_stop = 1;
}

static double timespec_diff_sec(const struct timespec *a, const struct timespec *b) {
    return (double)(a->tv_sec - b->tv_sec) + (double)(a->tv_nsec - b->tv_nsec) / 1e9;
}

int main(int argc, char **argv) {
    double wheel_speed = 20.0;
    int step_ms = 50;
    double drive_gain = 0.42;
    double damping = 0.18;

    if (argc > 1) {
        wheel_speed = atof(argv[1]);
    }
    if (argc > 2) {
        step_ms = atoi(argv[2]);
        if (step_ms <= 0) {
            fprintf(stderr, "step_ms must be > 0\n");
            return 1;
        }
    }
    if (argc > 3) {
        drive_gain = atof(argv[3]);
        if (drive_gain <= 0.0) {
            fprintf(stderr, "drive_gain must be > 0\n");
            return 1;
        }
    }
    if (argc > 4) {
        damping = atof(argv[4]);
        if (damping < 0.0) {
            fprintf(stderr, "damping must be >= 0\n");
            return 1;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int torque_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (torque_sock < 0) {
        perror("socket(torque_sock)");
        return 1;
    }

    int reuse = 1;
    if (setsockopt(torque_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(torque_sock);
        return 1;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(PLANT_TORQUE_PORT);
    if (bind(torque_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        close(torque_sock);
        return 1;
    }

    int state_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (state_sock < 0) {
        perror("socket(state_sock)");
        close(torque_sock);
        return 1;
    }

    struct sockaddr_in sensor_dst;
    memset(&sensor_dst, 0, sizeof(sensor_dst));
    sensor_dst.sin_family = AF_INET;
    sensor_dst.sin_port = htons(PLANT_STATE_PORT);
    if (inet_pton(AF_INET, SENSOR_IP, &sensor_dst.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed\n");
        close(state_sock);
        close(torque_sock);
        return 1;
    }

    double applied_torque = 0.0;
    uint64_t ctrl_seq = 0;
    uint64_t plant_seq = 0;
    struct timespec last_tick;
    clock_gettime(CLOCK_MONOTONIC, &last_tick);

    printf("[plant] listen 0.0.0.0:%d -> sensor %s:%d init_speed=%.3f step_ms=%d\n",
           PLANT_TORQUE_PORT, SENSOR_IP, PLANT_STATE_PORT, wheel_speed, step_ms);

    char buf[128];
    while (!g_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(torque_sock, &rfds);

        struct timeval tv;
        tv.tv_sec = step_ms / 1000;
        tv.tv_usec = (step_ms % 1000) * 1000;
        int sel = select(torque_sock + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }

        if (sel > 0 && FD_ISSET(torque_sock, &rfds)) {
            struct sockaddr_storage src_addr;
            socklen_t src_len = sizeof(src_addr);
            ssize_t r = recvfrom(torque_sock, buf, sizeof(buf) - 1, 0,
                                 (struct sockaddr *)&src_addr, &src_len);
            if (r > 0) {
                buf[r] = '\0';
                unsigned long long seq_ull = 0;
                double torque_cmd = 0.0;
                if (sscanf(buf, "%llu %lf", &seq_ull, &torque_cmd) == 2) {
                    ctrl_seq = (uint64_t)seq_ull;
                    applied_torque = torque_cmd;
                }
            } else if (r < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("recvfrom");
                break;
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double dt = timespec_diff_sec(&now, &last_tick);
        if (dt <= 0.0 || dt > 1.0) {
            dt = (double)step_ms / 1000.0;
        }
        last_tick = now;

        /*
         * Wheel-speed plant: omega_dot = drive_gain * torque - damping * omega - rolling_res
         * rolling_res approximates nonlinear road/load drag.
         */
        double rolling_res = 0.01 * wheel_speed * fabs(wheel_speed);
        double accel = drive_gain * applied_torque - damping * wheel_speed - rolling_res;
        wheel_speed += accel * dt;
        if (wheel_speed < 0.0) {
            wheel_speed = 0.0;
        }

        char out[128];
        int n = snprintf(out, sizeof(out), "%llu %.6f",
                         (unsigned long long)plant_seq, wheel_speed);
        if (n < 0 || n >= (int)sizeof(out)) {
            fprintf(stderr, "snprintf overflow\n");
            break;
        }

        if (sendto(state_sock, out, (size_t)n, 0,
                   (struct sockaddr *)&sensor_dst, sizeof(sensor_dst)) < 0) {
            perror("sendto");
            break;
        }

        printf("[plant] pseq=%llu cseq=%llu torque=%.6f wheel_speed=%.6f accel=%.6f\n",
               (unsigned long long)plant_seq, (unsigned long long)ctrl_seq,
               applied_torque, wheel_speed, accel);
        fflush(stdout);
        plant_seq++;
    }

    close(state_sock);
    close(torque_sock);
    return 0;
}
