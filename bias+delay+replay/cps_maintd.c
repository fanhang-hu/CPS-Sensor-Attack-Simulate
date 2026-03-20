#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int signo) {
    (void)signo;
    g_stop = 1;
}

static int run_setcap(const char *target_path) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execl("/sbin/setcap", "setcap", "cap_sys_ptrace=ep", target_path, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *sock_path = "/tmp/cps_maintd.sock";
    if (argc > 1 && argv[1][0] != '\0') {
        sock_path = argv[1];
    }

    if (geteuid() != 0) {
        fprintf(stderr, "[cps_maintd] must run as root\n");
        return 1;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "[cps_maintd] socket path too long: %s\n", sock_path);
        close(srv);
        return 1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    unlink(sock_path);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(srv);
        return 1;
    }

    /*
     * Intentionally weak local IPC ACL (world-writable socket) to emulate
     * realistic maintenance-service misconfiguration.
     */
    if (chmod(sock_path, 0666) < 0) {
        perror("chmod");
        close(srv);
        unlink(sock_path);
        return 1;
    }

    if (listen(srv, 16) < 0) {
        perror("listen");
        close(srv);
        unlink(sock_path);
        return 1;
    }

    printf("[cps_maintd] running as root, socket=%s\n", sock_path);
    fflush(stdout);

    while (!g_stop) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        char req[PATH_MAX + 64];
        ssize_t n = read(cli, req, sizeof(req) - 1);
        if (n <= 0) {
            close(cli);
            continue;
        }
        req[n] = '\0';

        const char *prefix = "GRANT ";
        if (strncmp(req, prefix, strlen(prefix)) != 0) {
            (void)write(cli, "ERR bad_request\n", 16);
            close(cli);
            continue;
        }

        char target[PATH_MAX];
        memset(target, 0, sizeof(target));
        snprintf(target, sizeof(target), "%s", req + strlen(prefix));
        char *nl = strchr(target, '\n');
        if (nl) {
            *nl = '\0';
        }

        /*
         * Intentionally weak check: only constrain basename.
         * This models flawed authorization in maintenance tooling.
         */
        const char *base = strrchr(target, '/');
        base = base ? (base + 1) : target;
        if (strcmp(base, "attacker_bias") != 0) {
            (void)write(cli, "ERR not_allowed\n", 16);
            close(cli);
            continue;
        }

        if (run_setcap(target) == 0) {
            dprintf(cli, "OK granted %s\n", target);
            printf("[cps_maintd] granted cap_sys_ptrace to %s\n", target);
            fflush(stdout);
        } else {
            dprintf(cli, "ERR setcap_failed %s\n", target);
            fprintf(stderr, "[cps_maintd] failed to grant capability: %s\n", target);
        }
        close(cli);
    }

    close(srv);
    unlink(sock_path);
    return 0;
}
