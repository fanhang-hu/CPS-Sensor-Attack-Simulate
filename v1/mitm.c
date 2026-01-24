// mitm.c - syscall-visible MITM for CPS demo (loopback UDP)
// Attacks: pass, bias, delay, replay, jitter, drop, scale, saturate
// Optional: tamper controller gains file (file-level attack steps)
//
// Build: gcc -O2 -Wall -o mitm mitm.c

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char* HOST = "127.0.0.1";
static const int MITM_PORT = 9001;  // listen y_raw
static const int CTRL_PORT = 9003;  // forward y_spoofed to controller
static const char* GAIN_PATH = "/tmp/cps_bench_gains.json";

#pragma pack(push, 1)
typedef struct {
    double ts;
    float  val;
    uint32_t seq;
} msg_t;
#pragma pack(pop)

typedef struct {
    msg_t m;
    int   used;
    uint64_t due_ns;
} slot_t;

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void msleep(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int write_atomic(const char* path, const char* content) {
    // write to temp then rename (generates open/write/fsync/rename syscalls)
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t n = write(fd, content, (ssize_t)strlen(content));
    (void)n;
    fsync(fd);
    close(fd);
    return rename(tmp, path);
}

static void maybe_tamper_gains(int enable) {
    if (!enable) return;

    // leave a marker file (helps provenance path)
    write_atomic("/tmp/cps_bench_attack_marker.txt", "MITM tampering gains\n");

    // set extreme gains to destabilize control (safe: only local demo)
    const char* payload =
        "{\n"
        "  \"Kp\": 6.0,\n"
        "  \"Ki\": 2.5,\n"
        "  \"u_min\": -10.0,\n"
        "  \"u_max\": 10.0\n"
        "}\n";
    write_atomic(GAIN_PATH, payload);

    // optional extra: exec a harmless helper to produce execve edge
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", "echo helper_executed >> /tmp/cps_bench_attack_marker.txt", (char*)NULL);
        _exit(0);
    }
}

static int rand_range(int lo, int hi) { // inclusive
    if (hi <= lo) return lo;
    return lo + (rand() % (hi - lo + 1));
}

int main(int argc, char** argv) {
    const char* mode = "pass";
    float bias = 1.5f;
    int delay_ms = 150;
    int jitter_ms = 40;
    float drop_p = 0.1f;
    float scale = 1.2f;
    float sat_lo = -5.0f, sat_hi = 5.0f;
    int tamper_gains = 0;

    // simple argv parsing
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--bias") && i + 1 < argc) bias = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--delay-ms") && i + 1 < argc) delay_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--jitter-ms") && i + 1 < argc) jitter_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--drop-p") && i + 1 < argc) drop_p = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--sat-lo") && i + 1 < argc) sat_lo = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--sat-hi") && i + 1 < argc) sat_hi = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--tamper-gains")) tamper_gains = 1;
    }

    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    // ensure default gains file exists (normal baseline)
    const char* default_gains =
        "{\n"
        "  \"Kp\": 1.2,\n"
        "  \"Ki\": 0.6,\n"
        "  \"u_min\": -10.0,\n"
        "  \"u_max\": 10.0\n"
        "}\n";
    struct stat st;
    if (stat(GAIN_PATH, &st) != 0) write_atomic(GAIN_PATH, default_gains);

    maybe_tamper_gains(tamper_gains);

    // socket setup
    int s_in = socket(AF_INET, SOCK_DGRAM, 0);
    int s_out = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_in < 0 || s_out < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr_in;
    memset(&addr_in, 0, sizeof(addr_in));
    addr_in.sin_family = AF_INET;
    addr_in.sin_port = htons(MITM_PORT);
    inet_pton(AF_INET, HOST, &addr_in.sin_addr);

    if (bind(s_in, (struct sockaddr*)&addr_in, sizeof(addr_in)) < 0) {
        perror("bind");
        return 1;
    }

    struct sockaddr_in addr_out;
    memset(&addr_out, 0, sizeof(addr_out));
    addr_out.sin_family = AF_INET;
    addr_out.sin_port = htons(CTRL_PORT);
    inet_pton(AF_INET, HOST, &addr_out.sin_addr);

    // delay queue
    const int QN = 2048;
    slot_t* Q = (slot_t*)calloc((size_t)QN, sizeof(slot_t));
    if (!Q) return 1;

    // replay buffer
    const int RN = 256;
    msg_t* R = (msg_t*)calloc((size_t)RN, sizeof(msg_t));
    int r_cnt = 0;

    // MITM log (file syscalls)
    const char* log_dir = "/tmp/cps_bench_logs";
    mkdir(log_dir, 0755);
    char log_path[256];
    snprintf(log_path, sizeof(log_path), "%s/mitm.csv", log_dir);
    FILE* lf = fopen(log_path, "w");
    if (lf) fprintf(lf, "t,seq,in_val,out_val,mode\n");

    struct pollfd pfd[1];
    pfd[0].fd = s_in;
    pfd[0].events = POLLIN;

    while (1) {
        uint64_t tnow = now_ns();

        // find next due packet
        uint64_t next_due = 0;
        for (int i = 0; i < QN; i++) {
            if (Q[i].used) {
                if (next_due == 0 || Q[i].due_ns < next_due) next_due = Q[i].due_ns;
            }
        }

        int timeout_ms = 50; // default
        if (next_due != 0) {
            if (next_due <= tnow) timeout_ms = 0;
            else {
                uint64_t diff = next_due - tnow;
                timeout_ms = (int)(diff / 1000000ull);
                if (timeout_ms < 0) timeout_ms = 0;
                if (timeout_ms > 50) timeout_ms = 50;
            }
        }

        int pr = poll(pfd, 1, timeout_ms);
        if (pr < 0 && errno != EINTR) {
            perror("poll");
            break;
        }

        // receive new packet
        if (pr > 0 && (pfd[0].revents & POLLIN)) {
            msg_t m;
            struct sockaddr_in src;
            socklen_t sl = sizeof(src);
            ssize_t n = recvfrom(s_in, &m, sizeof(m), 0, (struct sockaddr*)&src, &sl);
            if (n == (ssize_t)sizeof(m)) {
                // store to replay buffer
                R[r_cnt % RN] = m;
                r_cnt++;

                // maybe drop
                if (!strcmp(mode, "drop")) {
                    float r = (float)rand() / (float)RAND_MAX;
                    if (r < drop_p) continue;
                }

                float in_v = m.val;
                float out_v = in_v;

                if (!strcmp(mode, "bias")) out_v = in_v + bias;
                else if (!strcmp(mode, "scale")) out_v = in_v * scale;
                else if (!strcmp(mode, "saturate")) {
                    out_v = in_v;
                    if (out_v < sat_lo) out_v = sat_lo;
                    if (out_v > sat_hi) out_v = sat_hi;
                } else if (!strcmp(mode, "replay")) {
                    // send an older sample (simple)
                    if (r_cnt > 10) {
                        int back = rand_range(5, 30);
                        msg_t old = R[(r_cnt - back + RN) % RN];
                        out_v = old.val;
                        m.seq = old.seq; // make replay visible
                    }
                } else {
                    // pass / delay / jitter handled by due time only
                    out_v = in_v;
                }

                m.val = out_v;

                int extra = 0;
                if (!strcmp(mode, "delay")) extra = delay_ms;
                else if (!strcmp(mode, "jitter")) extra = rand_range(0, jitter_ms);

                uint64_t due = now_ns() + (uint64_t)extra * 1000000ull;

                // enqueue
                int placed = 0;
                for (int i = 0; i < QN; i++) {
                    if (!Q[i].used) {
                        Q[i].used = 1;
                        Q[i].m = m;
                        Q[i].due_ns = due;
                        placed = 1;
                        break;
                    }
                }
                if (!placed) {
                    // queue full -> drop silently (still syscall-visible via recvfrom)
                }

                if (lf) {
                    fprintf(lf, "%ld,%u,%.6f,%.6f,%s\n", time(NULL), m.seq, in_v, out_v, mode);
                    fflush(lf);
                }
            }
        }

        // send due packets
        tnow = now_ns();
        for (int i = 0; i < QN; i++) {
            if (Q[i].used && Q[i].due_ns <= tnow) {
                sendto(s_out, &Q[i].m, sizeof(Q[i].m), 0, (struct sockaddr*)&addr_out, sizeof(addr_out));
                Q[i].used = 0;
            }
        }
    }

    if (lf) fclose(lf);
    close(s_in);
    close(s_out);
    free(Q);
    free(R);
    return 0;
}
