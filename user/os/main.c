#include <libkario.h>
#define COPY_BUFFER 65536
#define INSTALL_MARKER "KarioBSD root filesystem 1\n"
static volatile sig_atomic_t interrupted;
static unsigned files_copied;
static const char *active_target;
static void stop_copy(int signo)
{
    interrupted = 1;
}
static int join_path(char *output, const char *root, const char *relative)
{
    int count =
        snprintf(output, PATH_MAX, "%s%s%s", root, *relative == '/' || !*root ? "" : "/", relative);
    if (count < 0 || count >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}
static int write_all(int fd, const void *bytes, size_t size)
{
    size_t done = 0;
    while (done < size) {
        if (interrupted) {
            errno = EINTR;
            return -1;
        }
        ssize_t count = write(fd, (const uint8_t *)bytes + done, size - done);
        if (count <= 0) {
            if (!count)
                errno = EIO;
            return -1;
        }
        done += count;
    }
    return 0;
}
static int open_directory(const char *path, unsigned mode, bool create)
{
    if (create && mkdir(path, mode) && errno != EEXIST)
        return -1;
    return open(path, O_RDONLY | O_DIRECTORY | O_NOSYMLINKS | O_CLOEXEC);
}
static int partition_root(const char *device, char *root)
{
    if (strncmp(device, "/dev/", 5) || !device[5] || strlen(device + 5) >= 32) {
        errno = EINVAL;
        return -1;
    }
    for (const char *p = device + 5; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) {
            errno = EINVAL;
            return -1;
        }
    }
    if (!mountpoint(device, root, PATH_MAX)) {
        if (!strcmp(root, "/")) {
            errno = EBUSY;
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT)
        return -1;
    int count = snprintf(root, PATH_MAX, "/mnt/%s", device + 5);
    if (count < 0 || count >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int directory = open_directory(root, 0755, true);
    if (directory < 0)
        return -1;
    close(directory);
    if (mount(device, root)) {
        if (errno != EBUSY)
            return -1;
    }
    return mountpoint(device, root, PATH_MAX);
}
static uint64_t checksum(uint64_t sum, const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++)
        sum = (sum ^ bytes[i]) * 1099511628211UL;
    return sum;
}
static int copy_regular(const char *source, const char *target, const struct kstat *stat)
{
    int input = open(source, O_RDONLY | O_NOSYMLINKS | O_CLOEXEC);
    if (input < 0)
        return -1;
    int output = open(target, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600U);
    if (output < 0 && errno == EEXIST) {
        output = open(target, O_WRONLY | O_NONBLOCK | O_NOSYMLINKS | O_CLOEXEC);
        if (output >= 0) {
            struct kstat existing;
            if (fstat(output, &existing) || (existing.mode & S_IFMT) != S_IFREG) {
                close(output);
                close(input);
                errno = EINVAL;
                return -1;
            }
            if (ftruncate(output, 0)) {
                int error = errno;
                close(output);
                close(input);
                errno = error;
                return -1;
            }
        }
    }
    if (output < 0) {
        int error = errno;
        close(input);
        errno = error;
        return -1;
    }
    uint8_t *buffer = malloc(COPY_BUFFER);
    if (!buffer) {
        close(input);
        close(output);
        errno = ENOMEM;
        return -1;
    }
    uint64_t expected = 14695981039346656037UL, total = 0;
    int result = 0;
    for (;;) {
        if (interrupted) {
            errno = EINTR;
            result = -1;
            break;
        }
        ssize_t count = read(input, buffer, COPY_BUFFER);
        if (count < 0) {
            result = -1;
            break;
        }
        if (!count)
            break;
        if (write_all(output, buffer, count)) {
            result = -1;
            break;
        }
        total += count;
        expected = checksum(expected, buffer, count);
    }
    int error = errno;
    if (!result && total != stat->size) {
        result = -1;
        error = EIO;
    }
    close(input);
    if (close(output) && !result) {
        result = -1;
        error = errno;
    }
    if (!result) {
        input = open(target, O_RDONLY | O_NOSYMLINKS | O_CLOEXEC);
        if (input < 0) {
            result = -1;
            error = errno;
        } else {
            uint64_t actual = 14695981039346656037UL;
            total = 0;
            for (;;) {
                ssize_t count = read(input, buffer, COPY_BUFFER);
                if (count < 0) {
                    result = -1;
                    error = errno;
                    break;
                }
                if (!count)
                    break;
                actual = checksum(actual, buffer, count);
                total += count;
            }
            close(input);
            if (!result && (actual != expected || total != stat->size)) {
                result = -1;
                error = EIO;
            }
        }
    }
    free(buffer);
    if (!result && (chown(target, stat->uid, stat->gid) || chmod(target, stat->mode & 07777))) {
        result = -1;
        error = errno;
    }
    errno = error;
    return result;
}
struct copy_paths {
    char first[PATH_MAX], second[PATH_MAX];
};
static int copy_tree(const char *, const char *, unsigned);
static int copy_tree_inner(const char *source, const char *target, unsigned depth,
                           struct copy_paths *paths)
{
    if (interrupted) {
        errno = EINTR;
        return -1;
    }
    if (depth >= 64) {
        errno = ELOOP;
        return -1;
    }
    if (active_target && !strcmp(source, active_target))
        return 0;
    char *link_target = paths->first;
    ssize_t length = readlink(source, link_target, PATH_MAX - 1);
    if (length >= 0) {
        link_target[length] = 0;
        char *existing = paths->second;
        ssize_t size = readlink(target, existing, PATH_MAX - 1);
        if (size >= 0) {
            existing[size] = 0;
            if (!strcmp(existing, link_target))
                return 0;
            if (unlink(target))
                return -1;
        } else if (errno != ENOENT) {
            return -1;
        }
        return symlink(link_target, target);
    }
    if (errno != EINVAL)
        return -1;
    int input = open(source, O_RDONLY | O_NONBLOCK | O_NOSYMLINKS | O_CLOEXEC);
    if (input < 0)
        return -1;
    struct kstat stat;
    if (fstat(input, &stat)) {
        int error = errno;
        close(input);
        errno = error;
        return -1;
    }
    if ((stat.mode & S_IFMT) == S_IFREG) {
        close(input);
        if (copy_regular(source, target, &stat))
            return -1;
        files_copied++;
        return 0;
    }
    if (!S_ISDIR(stat.mode)) {
        close(input);
        errno = ENOTSUP;
        return -1;
    }
    int directory = open_directory(target, 0755, true);
    if (directory < 0) {
        int error = errno;
        close(input);
        errno = error;
        return -1;
    }
    struct kdirent entry;
    int result;
    while ((result = readdir(input, &entry)) > 0) {
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, ".."))
            continue;
        char *from = paths->first, *to = paths->second;
        if (join_path(from, source, entry.name) || join_path(to, target, entry.name) ||
            copy_tree(from, to, depth + 1)) {
            fprintf(stderr, "os-install: failed copying %s\n", source);
            result = -1;
            break;
        }
    }
    int error = errno;
    close(input);
    if (result >= 0 && (chown(target, stat.uid, stat.gid) || chmod(target, stat.mode & 07777))) {
        result = -1;
        error = errno;
    }
    close(directory);
    errno = error;
    return result < 0 ? -1 : 0;
}
static int copy_tree(const char *source, const char *target, unsigned depth)
{
    struct copy_paths *paths = malloc(sizeof(*paths));
    if (!paths) {
        errno = ENOMEM;
        return -1;
    }
    int result = copy_tree_inner(source, target, depth, paths);
    int error = errno;
    free(paths);
    errno = error;
    return result;
}
static int validate_programs(const char *root)
{
    char path[PATH_MAX];
    if (join_path(path, root, "/etc/os-programs"))
        return -1;
    int fd = open(path, O_RDONLY | O_NOSYMLINKS | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct kstat stat;
    if (fstat(fd, &stat) || stat.size > 32768 || !stat.size) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    char *manifest = malloc(stat.size + 1);
    if (!manifest) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    size_t done = 0;
    while (done < stat.size) {
        ssize_t count = read(fd, manifest + done, stat.size - done);
        if (count <= 0) {
            close(fd);
            free(manifest);
            if (!count)
                errno = EIO;
            return -1;
        }
        done += count;
    }
    close(fd);
    manifest[done] = 0;
    int result = 0;
    bool shell = false, init = false;
    char *line = manifest;
    while (*line) {
        char *end = strchr(line, '\n');
        if (!end) {
            result = -1;
            errno = EINVAL;
            break;
        }
        *end = 0;
        if (strncmp(line, "/bin/", 5) && strncmp(line, "/sbin/", 6)) {
            result = -1;
            errno = EINVAL;
            break;
        }
        const char *name = strrchr(line, '/');
        if (!name || !name[1] || strchr(line + 1, '/') != name || !strcmp(name + 1, ".") ||
            !strcmp(name + 1, "..") || join_path(path, root, line)) {
            result = -1;
            errno = EINVAL;
            break;
        }
        int program = open(path, O_RDONLY | O_NOSYMLINKS | O_CLOEXEC);
        if (program < 0) {
            result = -1;
            break;
        }
        uint8_t header[20];
        ssize_t size = read(program, header, sizeof(header));
        int failed = fstat(program, &stat);
        close(program);
        if (failed || size != sizeof(header) || (stat.mode & S_IFMT) != S_IFREG ||
            !(stat.mode & 0111) ||
            memcmp(header,
                   "\x7f"
                   "ELF\x02\x01\x01",
                   7) ||
            header[18] != 62 || header[19]) {
            result = -1;
            errno = ENOEXEC;
            break;
        }
        shell |= !strcmp(line, "/bin/sh");
        init |= !strcmp(line, "/sbin/init");
        line = end + 1;
    }
    if (!result && (!shell || !init)) {
        result = -1;
        errno = EINVAL;
    }
    free(manifest);
    return result;
}
static int install_system(const char *device)
{
    if (validate_programs("")) {
        perror("os-install source");
        return 1;
    }
    char root[PATH_MAX];
    if (partition_root(device, root)) {
        perror("os-install partition");
        return 1;
    }
    char lock[PATH_MAX], marker[PATH_MAX];
    if (join_path(lock, root, "/.os-install.lock") ||
        join_path(marker, root, "/.kario-installed")) {
        perror("os-install path");
        return 1;
    }
    int exclusive = open(lock, O_WRONLY | O_CREAT | O_EXCL, 0600U);
    if (exclusive < 0) {
        perror("os-install lock");
        return 1;
    }
    close(exclusive);
    signal(SIGINT, stop_copy);
    signal(SIGTERM, stop_copy);
    int result = 0;
    if (unlink(marker) && errno != ENOENT)
        result = -1;
    active_target = root;
    const char *trees[] = {"/bin", "/sbin", "/etc", "/usr"};
    printf("os-install: copying system to %s (%s)\n", device, root);
    for (unsigned i = 0; !result && i < sizeof(trees) / sizeof(trees[0]); i++) {
        char target[PATH_MAX];
        if (join_path(target, root, trees[i]) || copy_tree(trees[i], target, 0))
            result = -1;
    }
    const char *directories[] = {"/dev", "/tmp", "/mnt", "/home", "/var"};
    for (unsigned i = 0; !result && i < sizeof(directories) / sizeof(directories[0]); i++) {
        char target[PATH_MAX];
        if (join_path(target, root, directories[i])) {
            result = -1;
            break;
        }
        unsigned mode = !strcmp(directories[i], "/tmp") ? 01777 : 0755;
        int directory = open_directory(target, mode, true);
        if (directory < 0) {
            result = -1;
            break;
        }
        close(directory);
        if (chmod(target, mode))
            result = -1;
    }
    if (!result)
        result = validate_programs(root);
    int error = errno;
    if (!result) {
        int fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0644U);
        if (fd < 0) {
            result = -1;
            error = errno;
        } else {
            result = write_all(fd, INSTALL_MARKER, sizeof(INSTALL_MARKER) - 1);
            error = errno;
            if (close(fd) && !result) {
                result = -1;
                error = errno;
            }
            if (result)
                unlink(marker);
        }
    }
    if (unlink(lock) && !result) {
        result = -1;
        error = errno;
    }
    if (result) {
        errno = error;
        perror("os-install incomplete");
        fprintf(stderr, "os-install: partial files remain on %s\n", device);
        return 1;
    }
    printf("os-install: installed and verified %u files; enter with os-init %s\n", files_copied,
           device);
    return 0;
}
static int enter_system(const char *device)
{
    char root[PATH_MAX], path[PATH_MAX];
    if (partition_root(device, root) || join_path(path, root, "/.os-install.lock")) {
        perror("os-init partition");
        return 1;
    }
    int lock = open(path, O_RDONLY | O_NOSYMLINKS);
    if (lock >= 0) {
        close(lock);
        fprintf(stderr, "os-init: installation is in progress or left an installation lock\n");
        return 1;
    }
    if (errno != ENOENT || join_path(path, root, "/.kario-installed")) {
        perror("os-init installation");
        return 1;
    }
    int fd = open(path, O_RDONLY | O_NOSYMLINKS);
    char marker[sizeof(INSTALL_MARKER)];
    ssize_t size = fd >= 0 ? read(fd, marker, sizeof(marker)) : -1;
    if (fd >= 0)
        close(fd);
    if (size != sizeof(INSTALL_MARKER) - 1 ||
        memcmp(marker, INSTALL_MARKER, sizeof(INSTALL_MARKER) - 1)) {
        fprintf(stderr, "os-init: partition does not contain a completed KarioBSD installation\n");
        return 1;
    }
    if (validate_programs(root)) {
        perror("os-init programs");
        return 1;
    }
    if (join_path(path, root, "/dev"))
        return 1;
    int directory = open_directory(path, 0755, false);
    if (directory < 0) {
        perror("os-init dev");
        return 1;
    }
    close(directory);
    if (mount("devfs", path) && errno != EBUSY) {
        perror("os-init devfs");
        return 1;
    }
    printf("os-init: entering %s (%s)\n", device, root);
    if (chroot(root) || chdir("/")) {
        perror("os-init chroot");
        return 1;
    }
    char *shell[] = {"/bin/sh", NULL};
    execve(shell[0], shell, environ);
    perror("os-init shell");
    return 1;
}
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s /dev/PARTITION\n", OS_INSTALLER ? "os-install" : "os-init");
        return 1;
    }
    if (getuid()) {
        fprintf(stderr, "%s: root privileges required\n", OS_INSTALLER ? "os-install" : "os-init");
        return 1;
    }
    return OS_INSTALLER ? install_system(argv[1]) : enter_system(argv[1]);
}
