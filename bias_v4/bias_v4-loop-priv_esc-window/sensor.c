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
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int signo) {
    (void)signo;
    g_stop = 1;
}

static int use_plant_mode(void) {
    const char *mode = getenv("CPS_SENSOR_MODE");
    if (!mode || mode[0] == '\0') {
        return 0;
    }
    return strcmp(mode, "plant") == 0;
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

    int plant_mode = use_plant_mode();
    uint64_t seq = 0;
    if (!plant_mode) {
        printf("[sensor] mode=synthetic -> controller %s:%d\n", SENSOR_IP, SENSOR_PORT);
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
    } else {
        int recv_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (recv_sock < 0) {
            perror("socket(recv_sock)");
            close(sockfd);
            return 1;
        }

        int reuse = 1;
        if (setsockopt(recv_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            perror("setsockopt(SO_REUSEADDR)");
            close(recv_sock);
            close(sockfd);
            return 1;
        }

        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind_addr.sin_port = htons(PLANT_STATE_PORT);
        if (bind(recv_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            perror("bind");
            close(recv_sock);
            close(sockfd);
            return 1;
        }

        printf("[sensor] mode=plant -> listen 0.0.0.0:%d, send controller %s:%d\n",
               PLANT_STATE_PORT, SENSOR_IP, SENSOR_PORT);

        char inbuf[128];
        while (!g_stop) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(recv_sock, &rfds);

            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            int sel = select(recv_sock + 1, &rfds, NULL, NULL, &tv);
            if (sel < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("select");
                break;
            }
            if (sel == 0) {
                continue;
            }

            struct sockaddr_storage src_addr;
            socklen_t src_len = sizeof(src_addr);
            ssize_t r = recvfrom(recv_sock, inbuf, sizeof(inbuf) - 1, 0,
                                 (struct sockaddr *)&src_addr, &src_len);
            if (r < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                perror("recvfrom");
                break;
            }
            inbuf[r] = '\0';

            unsigned long long plant_seq = 0;
            double wheel_speed = 0.0;
            if (sscanf(inbuf, "%llu %lf", &plant_seq, &wheel_speed) != 2) {
                fprintf(stderr, "[sensor] parse error: %s\n", inbuf);
                continue;
            }

            double meas_noise = sin((double)plant_seq / 7.0) * amplitude;
            double measured = wheel_speed + meas_noise;
            char out[128];
            int n = snprintf(out, sizeof(out), "%llu %.6f", plant_seq, measured);
            if (n < 0 || n >= (int)sizeof(out)) {
                fprintf(stderr, "snprintf overflow\n");
                break;
            }

            if (sendto(sockfd, out, (size_t)n, 0,
                       (struct sockaddr *)&dst, sizeof(dst)) < 0) {
                perror("sendto");
                break;
            }

            printf("[sensor] seq=%llu plant=%.6f measured=%.6f noise=%.6f\n",
                   plant_seq, wheel_speed, measured, meas_noise);
            fflush(stdout);
            usleep((useconds_t)interval_ms * 1000U);
        }

        close(recv_sock);
    }

    close(sockfd);
    return 0;
}
