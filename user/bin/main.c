#include <libkario.h>
static int copy_file(int in, int out)
{
    char buf[4096];
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0)
            return -1;
        if (!n)
            return 0;
        ssize_t done = 0;
        while (done < n) {
            ssize_t r = write(out, buf + done, n - done);
            if (r <= 0)
                return -1;
            done += r;
        }
    }
}
static int stat_path(const char *path, struct kstat *st)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int r = fstat(fd, st);
    int saved = errno;
    close(fd);
    errno = saved;
    return r;
}
static int list(const char *path, bool detailed)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    struct kdirent d;
    int r, result = 0;
    while ((r = readdir(fd, &d)) > 0) {
        if (!strcmp(d.name, ".") || !strcmp(d.name, ".."))
            continue;
        if (detailed) {
            char full[PATH_MAX];
            if (snprintf(full, sizeof(full), "%s/%s", path, d.name) >= PATH_MAX) {
                fprintf(stderr, "ls: path too long\n");
                result = 1;
                continue;
            }
            struct kstat st;
            if (stat_path(full, &st)) {
                perror(full);
                result = 1;
                continue;
            }
            printf("%06o %u %u %lu %s\n", st.mode, st.uid, st.nlink, st.size, d.name);
        } else
            printf("%s%s\n", d.name, S_ISDIR(d.mode) ? "/" : "");
    }
    if (r < 0) {
        perror("ls readdir");
        result = 1;
    }
    close(fd);
    return result;
}
static int remove_tree(const char *path, bool recursive, unsigned depth)
{
    if (depth > 64) {
        errno = ELOOP;
        return -1;
    }
    char target[PATH_MAX];
    if (readlink(path, target, sizeof(target)) >= 0)
        return unlink(path);
    struct kstat st;
    if (stat_path(path, &st))
        return -1;
    if (!S_ISDIR(st.mode))
        return unlink(path);
    if (!recursive)
        return rmdir(path);
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return -1;
    struct kdirent d;
    int r;
    while ((r = readdir(fd, &d)) > 0) {
        if (!strcmp(d.name, ".") || !strcmp(d.name, ".."))
            continue;
        char full[PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", path, d.name) >= PATH_MAX) {
            close(fd);
            errno = ENAMETOOLONG;
            return -1;
        }
        if (remove_tree(full, true, depth + 1)) {
            close(fd);
            return -1;
        }
        lseek(fd, 0, SEEK_SET);
    }
    close(fd);
    if (r < 0)
        return -1;
    return rmdir(path);
}
static int mkdir_parents(const char *path)
{
    if (strlen(path) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char p[PATH_MAX];
    strcpy(p, path);
    for (char *s = p + 1;; s++)
        if (*s == '/' || !*s) {
            char c = *s;
            *s = 0;
            if (mkdir(p, 0755U) && errno != EEXIST)
                return -1;
            *s = c;
            if (!c)
                break;
        }
    return 0;
}
int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "kill")) {
        int signo = SIGTERM, start = 1;
        if (argc > 1 && argv[1][0] == '-') {
            const char *value = argv[1] + 1;
            const char *names[] = {"HUP",  "INT",  "QUIT", "ILL",  "TRAP", "ABRT", "BUS",  "FPE",
                                   "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM", NULL};
            signo = atoi(value);
            for (unsigned i = 0; i < 15; i++)
                if (!strcmp(value, names[i]))
                    signo = i + 1;
            if (!strcmp(value, "STOP"))
                signo = SIGSTOP;
            else if (!strcmp(value, "CONT"))
                signo = SIGCONT;
            else if (!strcmp(value, "TSTP"))
                signo = SIGTSTP;
            if (!signo && strcmp(value, "0")) {
                fprintf(stderr, "kill: invalid signal\n");
                return 1;
            }
            start = 2;
        }
        if (argc <= start) {
            fprintf(stderr, "usage: kill [-SIGNAL] pid...\n");
            return 1;
        }
        int result = 0;
        for (int i = start; i < argc; i++) {
            char *end;
            long pid = strtol(argv[i], &end, 10);
            if (*end || pid < INT32_MIN || pid > INT32_MAX || kill(pid, signo)) {
                perror("kill");
                result = 1;
            }
        }
        return result;
    }
    if (!strcmp(name, "echo")) {
        int start = 1;
        bool newline = true;
        if (argc > 1 && !strcmp(argv[1], "-n")) {
            start = 2;
            newline = false;
        }
        for (int i = start; i < argc; i++) {
            if (i > start)
                putchar(' ');
            fputs(argv[i], stdout);
        }
        if (newline)
            putchar('\n');
        return ferror(stdout) ? 1 : 0;
    }
    if (!strcmp(name, "cat")) {
        if (argc == 1) {
            if (copy_file(0, 1)) {
                perror("cat");
                return 1;
            }
            return 0;
        }
        int status = 0;
        for (int i = 1; i < argc; i++) {
            int fd = !strcmp(argv[i], "-") ? 0 : open(argv[i], O_RDONLY);
            if (fd < 0) {
                perror(argv[i]);
                status = 1;
                continue;
            }
            if (copy_file(fd, 1)) {
                perror(argv[i]);
                status = 1;
            }
            if (fd)
                close(fd);
        }
        return status;
    }
    if (!strcmp(name, "ls")) {
        bool detailed = argc > 1 && !strcmp(argv[1], "-l");
        int start = detailed ? 2 : 1, status = 0;
        if (start == argc)
            return list(".", detailed);
        for (int i = start; i < argc; i++)
            status |= list(argv[i], detailed);
        return status;
    }
    if (!strcmp(name, "pwd")) {
        char p[PATH_MAX];
        if (!getcwd(p, sizeof(p))) {
            perror("pwd");
            return 1;
        }
        puts(p);
        return 0;
    }
    if (!strcmp(name, "mkdir")) {
        bool parents = argc > 1 && !strcmp(argv[1], "-p");
        int start = parents ? 2 : 1, status = 0;
        if (argc <= start) {
            fprintf(stderr, "usage: mkdir [-p] path...\n");
            return 1;
        }
        for (int i = start; i < argc; i++)
            if (parents ? mkdir_parents(argv[i]) : mkdir(argv[i], 0755U)) {
                perror(argv[i]);
                status = 1;
            }
        return status;
    }
    if (!strcmp(name, "rm")) {
        bool recursive = argc > 1 && (!strcmp(argv[1], "-r") || !strcmp(argv[1], "-rf"));
        int start = recursive ? 2 : 1, status = 0;
        if (argc <= start) {
            fprintf(stderr, "usage: rm [-r] path...\n");
            return 1;
        }
        for (int i = start; i < argc; i++)
            if (remove_tree(argv[i], recursive, 0)) {
                perror(argv[i]);
                status = 1;
            }
        return status;
    }
    if (!strcmp(name, "uname") || !strcmp(name, "meminfo") || !strcmp(name, "uptime")) {
        struct ksysinfo s;
        if (sysinfo(&s)) {
            perror(name);
            return 1;
        }
        if (!strcmp(name, "uname"))
            printf("KarioBSD %s x86_64\n", s.release);
        else if (!strcmp(name, "meminfo"))
            printf("Total: %lu KiB\nFree: %lu KiB\nReserved: %lu KiB\nCPUs: %u\n", s.total / 1024,
                   s.free / 1024, s.reserved / 1024, s.cpus);
        else
            printf("Uptime: %lu.%03lu seconds\n", s.uptime_ns / 1000000000UL,
                   (s.uptime_ns / 1000000UL) % 1000);
        return 0;
    }
    if (!strcmp(name, "ln")) {
        bool symbolic = argc > 1 && !strcmp(argv[1], "-s");
        int start = symbolic ? 2 : 1;
        if (argc != start + 2) {
            fprintf(stderr, "usage: ln [-s] target link\n");
            return 1;
        }
        if (symbolic ? symlink(argv[start], argv[start + 1]) : link(argv[start], argv[start + 1])) {
            perror(name);
            return 1;
        }
        return 0;
    }
    if (!strcmp(name, "chmod")) {
        if (argc < 3) {
            fprintf(stderr, "usage: chmod octal-mode path...\n");
            return 1;
        }
        char *end;
        errno = 0;
        long mode = strtol(argv[1], &end, 8);
        if (*end || errno || mode < 0 || mode > 07777) {
            fprintf(stderr, "chmod: invalid mode\n");
            return 1;
        }
        int status = 0;
        for (int i = 2; i < argc; i++)
            if (chmod(argv[i], mode)) {
                perror(argv[i]);
                status = 1;
            }
        return status;
    }
    if (!strcmp(name, "partscan")) {
        if (argc != 2) {
            fprintf(stderr, "usage: partscan /dev/disk\n");
            return 1;
        }
        if (partscan(argv[1])) {
            perror("partscan");
            return 1;
        }
        return 0;
    }
    if (!strcmp(name, "mount")) {
        if (argc != 3) {
            fprintf(stderr, "usage: mount /dev/disk mountpoint\n");
            return 1;
        }
        if (mount(argv[1], argv[2])) {
            perror("mount");
            return 1;
        }
        return 0;
    }
    if (!strcmp(name, "stat")) {
        if (argc < 2) {
            fprintf(stderr, "usage: stat path...\n");
            return 1;
        }
        int status = 0;
        for (int i = 1; i < argc; i++) {
            struct kstat s;
            if (stat_path(argv[i], &s)) {
                perror(argv[i]);
                status = 1;
            } else
                printf("%s: inode=%lu size=%lu mode=%06o links=%u uid=%u gid=%u\n", argv[i], s.ino,
                       s.size, s.mode, s.nlink, s.uid, s.gid);
        }
        return status;
    }
    if (!strcmp(name, "cp")) {
        if (argc != 3) {
            fprintf(stderr, "usage: cp source destination\n");
            return 1;
        }
        int src = open(argv[1], O_RDONLY);
        if (src < 0) {
            perror(argv[1]);
            return 1;
        }
        int dest = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644U);
        if (dest < 0) {
            perror(argv[2]);
            close(src);
            return 1;
        }
        int r = copy_file(src, dest);
        if (r)
            perror("cp");
        close(src);
        close(dest);
        return r ? 1 : 0;
    }
    if (!strcmp(name, "sleep")) {
        if (argc != 2) {
            fprintf(stderr, "usage: sleep seconds\n");
            return 1;
        }
        char *end;
        errno = 0;
        long sec = strtol(argv[1], &end, 10);
        if (*end || sec < 0 || errno) {
            fprintf(stderr, "sleep: invalid duration\n");
            return 1;
        }
        struct ktimespec t = {sec, 0};
        if (nanosleep(&t, NULL)) {
            perror("sleep");
            return 1;
        }
        return 0;
    }
    if (!strcmp(name, "true"))
        return 0;
    if (!strcmp(name, "false"))
        return 1;
    fprintf(stderr, "Unknown utility: %s\n", name);
    return 127;
}
