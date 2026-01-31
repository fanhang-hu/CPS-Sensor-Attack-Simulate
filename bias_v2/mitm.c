// mitm.c - ONLY bias MITM for CPS demo (FIFO relay)
// Purpose: make the bias attack *syscall-visible* via extra open/read/write
//
// Baseline (pass): sensor writes FIFO A, controller reads FIFO A. (No MITM process)
// Attack  (bias): sensor -> FIFO IN -> [MITM adds bias] -> FIFO OUT -> controller
//
// Build: gcc -O2 -Wall -o mitm mitm.c
//
// Usage example:
//   ./mitm --in /tmp/cps_bias_v2_bus/y_in.pipe --out /tmp/cps_bias_v2_bus/y_out.pipe --bias 1.5

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#pragma pack(push, 1)
typedef struct {
    double ts;
    float  val;
    uint32_t seq;
    unsigned char mac[32]; // keep original mac to let controller detect tamper
} msg_t;
#pragma pack(pop)

static ssize_t read_full(int fd, void* buf, size_t n) {
    unsigned char* p = (unsigned char*)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return 0;               // EOF
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static ssize_t write_full(int fd, const void* buf, size_t n) {
    const unsigned char* p = (const unsigned char*)buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return (ssize_t)sent;
}

int main(int argc, char** argv) {
    const char* in_path  = "/tmp/cps_bias_v2_bus/y_in.pipe";
    const char* out_path = "/tmp/cps_bias_v2_bus/y_out.pipe";
    float bias = 1.5f;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--in") && i + 1 < argc) in_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--bias") && i + 1 < argc) bias = (float)atof(argv[++i]);
    }

    // Open FIFO IN for reading (blocks until writer opens)
    int fd_in = open(in_path, O_RDONLY);
    if (fd_in < 0) {
        perror("open(in)");
        return 1;
    }

    // Open FIFO OUT for writing (blocks until reader opens)
    int fd_out = open(out_path, O_WRONLY);
    if (fd_out < 0) {
        perror("open(out)");
        return 1;
    }

    fprintf(stderr, "[mitm] bias=%.3f in=%s out=%s\n", bias, in_path, out_path);

    while (1) {
        msg_t m;
        ssize_t r = read_full(fd_in, &m, sizeof(m));
        if (r == 0) break;
        if (r < 0) {
            perror("read");
            break;
        }

        float in_v = m.val;
        m.val = in_v + bias;

        // NOTE: We intentionally DO NOT update m.mac, so controller can detect tampering.
        if (write_full(fd_out, &m, sizeof(m)) < 0) {
            perror("write");
            break;
        }
    }

    close(fd_in);
    close(fd_out);
    return 0;
}
