#include <kernel.h>
static spinlock_t partition_lock = SPINLOCK_INIT;
static int fd_alloc(struct task *t, struct file *f)
{
    for (unsigned i = 0; i < MAX_FDS; i++)
        if (!t->fds[i]) {
            t->fds[i] = f;
            return i;
        }
    return -EMFILE;
}
static struct file *fd_get(struct task *t, uint64_t fd)
{
    return fd < MAX_FDS ? t->fds[fd] : NULL;
}
static void block_retry(struct task *t, struct frame *f, uint64_t ns)
{
    spin_lock(&sched_lock);
    t->state = BLOCKED;
    t->wake = monotonic_ns() + ns;
    t->retrying = true;
    if (t->pending & ~t->signal_mask)
        t->state = READY;
    spin_unlock(&sched_lock);
    f->rip -= 2;
}
static int path_copy(char *p, uint64_t at)
{
    return copy_string(p, at, PATH_MAX);
}
static int cwd_path(struct vnode *node, char *out)
{
    char path[PATH_MAX] = {0};
    size_t used = 0;
    struct vnode *n = node;
    vnode_get(n);
    for (unsigned depth = 0; n != vfs_root && depth < 128; depth++) {
        struct vnode *parent = n->parent;
        if (!parent || parent == n) {
            vnode_put(n);
            return -ENOENT;
        }
        vnode_get(parent);
        struct kdirent d;
        bool found = false;
        for (uint64_t i = 0;; i++) {
            int r = parent->ops->readdir ? parent->ops->readdir(parent, i, &d) : -ENOTDIR;
            if (r <= 0)
                break;
            if (!strcmp(d.name, ".") || !strcmp(d.name, ".."))
                continue;
            struct vnode *child;
            if (!parent->ops->lookup(parent, d.name, &child)) {
                struct vnode *candidate = child->mounted ? child->mounted : child;
                found = candidate == n || (candidate->fs == n->fs && candidate->ino == n->ino);
                vnode_put(child);
                if (found)
                    break;
            }
        }
        if (!found) {
            vnode_put(parent);
            vnode_put(n);
            return -ENOENT;
        }
        size_t len = strlen(d.name);
        if (len + used + 2 > PATH_MAX) {
            vnode_put(parent);
            vnode_put(n);
            return -ENAMETOOLONG;
        }
        memmove(path + len + 1, path, used + 1);
        path[0] = '/';
        memcpy(path + 1, d.name, len);
        used += len + 1;
        vnode_put(n);
        n = parent;
    }
    if (n != vfs_root) {
        vnode_put(n);
        return -ELOOP;
    }
    vnode_put(n);
    if (!used)
        strcpy(path, "/");
    strcpy(out, path);
    return 0;
}
static int collect_args(uint64_t user, char **list)
{
    memset(list, 0, 33 * sizeof(*list));
    if (!user)
        return 0;
    for (unsigned i = 0; i < 33; i++) {
        uint64_t p;
        if (copy_in(&p, user + i * 8, 8))
            return -EFAULT;
        if (!p)
            return 0;
        if (i == 32)
            return -E2BIG;
        list[i] = kmalloc(4096);
        if (!list[i])
            return -ENOMEM;
        int r = copy_string(list[i], p, 4096);
        if (r)
            return r == -ENAMETOOLONG ? -E2BIG : r;
    }
    return -E2BIG;
}
static void release_args(char **list)
{
    for (unsigned i = 0; i < 32; i++)
        kfree(list[i]);
}
static int64_t syscall_run(struct task *t, struct frame *f, bool *resched)
{
    uint64_t a = f->rdi, b = f->rsi, c = f->rdx, d = f->r10, e = f->r8, g = f->r9;
    char path[PATH_MAX];
    struct vnode *n;
    int r;
    switch (f->rax) {
        case SYS_pipe2: {
            if (!user_range(t->root, a, 2 * sizeof(int), true))
                return -EFAULT;
            int descriptors[2];
            r = pipe_create(t, b, descriptors);
            return r ? r : copy_out(a, descriptors, sizeof(descriptors));
        }
        case SYS_fcntl: {
            struct file *file = fd_get(t, a);
            if (!file)
                return -EBADF;
            switch (b) {
                case F_GETFD:
                    return t->fd_flags[a];
                case F_SETFD:
                    if (c & ~FD_CLOEXEC)
                        return -EINVAL;
                    t->fd_flags[a] = c;
                    return 0;
                case F_GETFL:
                    return file->flags;
                case F_SETFL:
                    if (c & ~(O_NONBLOCK | O_APPEND | O_DIRECTORY | 3UL))
                        return -EINVAL;
                    spin_lock(&file->lock);
                    file->flags =
                        (file->flags & ~(O_NONBLOCK | O_APPEND)) | (c & (O_NONBLOCK | O_APPEND));
                    spin_unlock(&file->lock);
                    return 0;
                default:
                    return -EINVAL;
            }
        }
        case SYS_kill:
            return signal_kill((int)a, (int)b);
        case SYS_sigaction:
            return signal_action((int)a, b, c);
        case SYS_sigprocmask:
            return signal_mask((int)a, b, c);
        case SYS_sigpending:
            return signal_pending(a);
        case SYS_sigsuspend:
            r = signal_suspend(a);
            if (!r)
                *resched = true;
            return r ? r : -EINTR;
        case SYS_sigreturn:
            return signal_restore(a, f);
        case SYS_alarm: {
            uint64_t now = monotonic_ns();
            if (a > (UINT64_MAX - now) / 1000000000UL)
                return -EINVAL;
            spin_lock(&sched_lock);
            uint64_t previous =
                t->alarm_deadline > now ? (t->alarm_deadline - now + 999999999) / 1000000000UL : 0;
            t->alarm_deadline = a ? now + a * 1000000000UL : 0;
            spin_unlock(&sched_lock);
            return previous;
        }
        case SYS_getppid:
            return t->ppid;
        case SYS_setpgid:
            return process_group((int)a, (int)b, false);
        case SYS_getpgid: {
            spin_lock(&sched_lock);
            int group = -ESRCH;
            for (unsigned i = 0; i < MAX_TASKS; i++)
                if (tasks[i].pid == ((int)a ? (int)a : t->pid))
                    group = tasks[i].pgid;
            spin_unlock(&sched_lock);
            return group;
        }
        case SYS_setsid:
            return process_group(0, 0, true);
        case SYS_tcgetpgrp:
        case SYS_tcsetpgrp: {
            struct file *file = fd_get(t, a);
            if (!file)
                return -EBADF;
            struct vnode *console;
            r = vfs_lookup("/dev/console", t->cwd, true, &console);
            if (r)
                return r;
            bool match = console == file->node;
            vnode_put(console);
            if (!match)
                return -ENOTTY;
            return terminal_group((int)b, f->rax == SYS_tcsetpgrp);
        }
        case SYS_read:
        case SYS_write: {
            bool write = f->rax == SYS_write;
            struct file *file = fd_get(t, a);
            if (!file)
                return -EBADF;
            if ((file->flags & 3) == (write ? O_RDONLY : O_WRONLY))
                return -EBADF;
            if (c > INT32_MAX || !user_range(t->root, b, c, !write))
                return -EFAULT;
            if (!c)
                return 0;
            spin_lock(&file->lock);
            bool regular = write && (file->node->mode & S_IFMT) == S_IFREG;
            if (regular)
                spin_lock(&file->node->lock);
            uint64_t off = write && (file->flags & O_APPEND) ? file->node->size : file->offset;
            if (write)
                r = file->node->ops->write ? file->node->ops->write(file->node, off, (void *)b, c)
                                           : -ENOTSUP;
            else
                r = file->node->ops->read ? file->node->ops->read(file->node, off, (void *)b, c)
                                          : -ENOTSUP;
            if (r > 0)
                file->offset = off + r;
            if (regular)
                spin_unlock(&file->node->lock);
            spin_unlock(&file->lock);
            if (r == -EPIPE && write)
                signal_queue_self(SIGPIPE);
            if (r == -EAGAIN && !(file->flags & O_NONBLOCK)) {
                block_retry(t, f, 10000000);
                *resched = true;
                return write ? SYS_write : SYS_read;
            }
            return r;
        }
        case SYS_open: {
            r = path_copy(path, a);
            if (r)
                return r;
            struct file *file;
            r = vfs_open(path, t, b, c, &file);
            if (r)
                return r;
            r = fd_alloc(t, file);
            if (r >= 0)
                t->fd_flags[r] = b & O_CLOEXEC ? FD_CLOEXEC : 0;
            if (r < 0)
                file_put(file);
            return r;
        }
        case SYS_close: {
            struct file *file = fd_get(t, a);
            if (!file)
                return -EBADF;
            t->fds[a] = NULL;
            t->fd_flags[a] = 0;
            file_put(file);
            return 0;
        }
        case SYS_lseek: {
            struct file *file = fd_get(t, a);
            if (!file)
                return -EBADF;
            if ((file->node->mode & S_IFMT) == S_IFCHR || (file->node->mode & S_IFMT) == S_IFIFO)
                return -ESPIPE;
            if (c > 2)
                return -EINVAL;
            spin_lock(&file->lock);
            uint64_t base = c == SEEK_SET ? 0 : c == SEEK_CUR ? file->offset : file->node->size;
            int64_t delta = (int64_t)b;
            if ((delta < 0 && base < 0 - b) || (delta >= 0 && base > INT64_MAX - b)) {
                spin_unlock(&file->lock);
                return -EINVAL;
            }
            uint64_t pos = base + b;
            file->offset = pos;
            spin_unlock(&file->lock);
            return pos;
        }
        case SYS_fstat: {
            struct file *file = fd_get(t, a);
            if (!file)
                return -EBADF;
            n = file->node;
            struct kstat st = {.ino = n->ino,
                               .size = n->size,
                               .blocks = n->blocks,
                               .atime = n->atime,
                               .mtime = n->mtime,
                               .ctime = n->ctime,
                               .mode = n->mode,
                               .nlink = n->nlink,
                               .uid = n->uid,
                               .gid = n->gid};
            return copy_out(b, &st, sizeof(st));
        }
        case SYS_dup2: {
            struct file *file = fd_get(t, a);
            if (!file || b >= MAX_FDS)
                return -EBADF;
            if (a == b)
                return b;
            atomic_fetch_add(&file->refs, 1);
            file_put(t->fds[b]);
            t->fds[b] = file;
            t->fd_flags[b] = 0;
            return b;
        }
        case SYS_ftruncate: {
            struct file *file = fd_get(t, a);
            if (!file || (file->flags & 3) == O_RDONLY)
                return -EBADF;
            if ((int64_t)b < 0)
                return -EINVAL;
            return file->node->ops->truncate ? file->node->ops->truncate(file->node, b) : -ENOTSUP;
        }
        case SYS_getuid:
            return t->uid;
        case SYS_getgid:
            return t->gid;
        case SYS_setuid:
            if (a > UINT32_MAX)
                return -EINVAL;
            if (t->uid && t->uid != a)
                return -EPERM;
            t->uid = a;
            return 0;
        case SYS_setgid:
            if (a > UINT32_MAX)
                return -EINVAL;
            if (t->uid && t->gid != a)
                return -EPERM;
            t->gid = a;
            return 0;
        case SYS_umask: {
            unsigned old = t->umask;
            t->umask = a & 0777;
            return old;
        }
        case SYS_chown: {
            r = path_copy(path, a);
            if (r)
                return r;
            r = vfs_lookup(path, t->cwd, true, &n);
            if (r)
                return r;
            if (t->uid && (t->uid != n->uid || (b != UINT32_MAX && b != t->uid) ||
                           (c != UINT32_MAX && c != t->gid)))
                r = -EPERM;
            else
                r = n->ops->chown ? n->ops->chown(n, b, c) : -ENOTSUP;
            vnode_put(n);
            return r;
        }
        case SYS_getpid:
            return t->pid;
        case SYS_exit:
            task_exit((int)a);
            *resched = true;
            return 0;
        case SYS_yield:
            *resched = true;
            return 0;
        case SYS_nanosleep: {
            struct ktimespec ts;
            r = copy_in(&ts, a, sizeof(ts));
            if (r)
                return r;
            if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000 ||
                (uint64_t)ts.tv_sec > (UINT64_MAX - monotonic_ns() - ts.tv_nsec) / 1000000000UL)
                return -EINVAL;
            uint64_t ns = ts.tv_sec * 1000000000UL + ts.tv_nsec;
            if (!ns)
                return 0;
            if (b) {
                struct ktimespec zero = {0};
                if (copy_out(b, &zero, sizeof(zero)))
                    return -EFAULT;
            }
            spin_lock(&sched_lock);
            t->state = BLOCKED;
            t->wake = monotonic_ns() + ns;
            t->sleeping = true;
            t->sleep_deadline = t->wake;
            t->sleep_remaining = b;
            if (t->pending & ~t->signal_mask) {
                t->state = READY;
                t->wake = 0;
            }
            spin_unlock(&sched_lock);
            *resched = true;
            return 0;
        }
        case SYS_mmap: {
            if (!b || b > 64 * 1024 * 1024 || b > SIZE_MAX - 4095 || c & ~7UL || (c & 6) == 6 ||
                !c || d != (MAP_PRIVATE | MAP_ANONYMOUS) || (int64_t)e != -1 || g)
                return -EINVAL;
            uint64_t len = ALIGN_UP(b, 4096), base = a ? ALIGN_DOWN(a, 4096) : t->mmap_next;
            if (base < PAGE_SIZE || base >= USER_STACK - 65536 || len > USER_STACK - 65536 - base)
                return -ENOMEM;
            for (uint64_t v = base; v < base + len; v += 4096)
                if (page_translate(t->root, v))
                    return -EEXIST;
            uint64_t flags =
                         PTE_U | ((c & PROT_WRITE) ? PTE_W : 0) | ((c & PROT_EXEC) ? 0 : PTE_NX),
                     done = 0;
            for (; done < len; done += 4096) {
                uint64_t p = frame_alloc();
                if (!p)
                    break;
                r = page_map(t->root, base + done, p, flags);
                if (r) {
                    frame_put(p);
                    break;
                }
            }
            if (done < len) {
                while (done) {
                    done -= 4096;
                    page_unmap(t->root, base + done, true);
                }
                return -ENOMEM;
            }
            if (!a)
                t->mmap_next = base + len + 4096;
            return base;
        }
        case SYS_munmap: {
            if (a % 4096 || !b || a < PAGE_SIZE || a >= USER_TOP || b > USER_TOP - a ||
                b > SIZE_MAX - 4095)
                return -EINVAL;
            uint64_t len = ALIGN_UP(b, 4096);
            for (uint64_t v = a; v < a + len; v += 4096)
                page_unmap(t->root, v, true);
            return 0;
        }
        case SYS_execve: {
            r = path_copy(path, a);
            if (r)
                return r;
            char *argv[33] = {0}, *envp[33] = {0};
            r = collect_args(b, argv);
            if (!r)
                r = collect_args(c, envp);
            if (!r)
                r = task_exec(t, path, (const char *const *)argv, (const char *const *)envp);
            release_args(argv);
            release_args(envp);
            return r;
        }
        case SYS_fork:
            return task_fork(f);
        case SYS_waitpid: {
            if ((int64_t)a < -INT32_MAX || (int64_t)a > INT32_MAX)
                return -EINVAL;
            if (c & ~(WNOHANG | WUNTRACED | WCONTINUED))
                return -EINVAL;
            if (b && !user_range(t->root, b, sizeof(int), true))
                return -EFAULT;
            spin_lock(&sched_lock);
            struct task *child = NULL;
            bool exists = false;
            for (unsigned i = 0; i < MAX_TASKS; i++)
                if (tasks[i].pid && tasks[i].ppid == t->pid &&
                    ((int64_t)a == -1 || ((int)a > 0 && tasks[i].pid == (int)a) ||
                     ((int)a == 0 && tasks[i].pgid == t->pgid) ||
                     ((int)a < -1 && tasks[i].pgid == -(int)a))) {
                    exists = true;
                    if (((c & WUNTRACED) && tasks[i].stop_event) ||
                        ((c & WCONTINUED) && tasks[i].continue_event)) {
                        int pid = tasks[i].pid, status = tasks[i].stop_event && (c & WUNTRACED)
                                                             ? (tasks[i].waiting << 8) | 127
                                                             : 0xffff;
                        if (status == 0xffff)
                            tasks[i].continue_event = false;
                        else
                            tasks[i].stop_event = false;
                        spin_unlock(&sched_lock);
                        if (b)
                            copy_out(b, &status, sizeof(status));
                        return pid;
                    }
                    if (tasks[i].state == TERMINATED && tasks[i].retired) {
                        child = &tasks[i];
                        child->state = BLOCKED;
                        break;
                    }
                }
            if (child) {
                int status = child->status, pid = child->pid;
                spin_unlock(&sched_lock);
                if (b)
                    copy_out(b, &status, sizeof(status));
                task_reap(child);
                return pid;
            }
            spin_unlock(&sched_lock);
            if (!exists)
                return -ECHILD;
            if (c & WNOHANG)
                return 0;
            block_retry(t, f, 10000000);
            *resched = true;
            return SYS_waitpid;
        }
        case SYS_chdir: {
            r = path_copy(path, a);
            if (r)
                return r;
            r = vfs_lookup(path, t->cwd, true, &n);
            if (r)
                return r;
            if (!S_ISDIR(n->mode)) {
                vnode_put(n);
                return -ENOTDIR;
            }
            char real[PATH_MAX];
            r = cwd_path(n, real);
            if (!r) {
                vnode_put(t->cwd);
                t->cwd = n;
                strcpy(t->cwd_path, real);
            } else
                vnode_put(n);
            return r;
        }
        case SYS_getcwd: {
            r = cwd_path(t->cwd, path);
            if (r)
                return r;
            size_t len = strlen(path) + 1;
            if (b < len)
                return -ERANGE;
            r = copy_out(a, path, len);
            return r ? r : (int64_t)a;
        }
        case SYS_readdir: {
            struct file *file = fd_get(t, a);
            if (!file)
                return -EBADF;
            if (!S_ISDIR(file->node->mode))
                return -ENOTDIR;
            if (!user_range(t->root, b, sizeof(struct kdirent), true))
                return -EFAULT;
            spin_lock(&file->lock);
            struct kdirent de;
            r = file->node->ops->readdir(file->node, file->offset, &de);
            if (r > 0) {
                copy_out(b, &de, sizeof(de));
                file->offset++;
            }
            spin_unlock(&file->lock);
            return r;
        }
        case SYS_mkdir: {
            r = path_copy(path, a);
            if (r)
                return r;
            r = vfs_create(path, t->cwd, S_IFDIR | (b & 07777 & ~t->umask), NULL, &n);
            if (!r)
                vnode_put(n);
            return r;
        }
        case SYS_unlink:
        case SYS_rmdir: {
            r = path_copy(path, a);
            if (r)
                return r;
            return vfs_remove(path, t->cwd, f->rax == SYS_rmdir);
        }
        case SYS_symlink: {
            char target[PATH_MAX];
            r = path_copy(target, a);
            if (r)
                return r;
            r = path_copy(path, b);
            if (r)
                return r;
            if (!*target)
                return -ENOENT;
            r = vfs_create(path, t->cwd, S_IFLNK | 0777, target, &n);
            if (!r)
                vnode_put(n);
            return r;
        }
        case SYS_readlink: {
            r = path_copy(path, a);
            if (r)
                return r;
            r = vfs_lookup(path, t->cwd, false, &n);
            if (r)
                return r;
            if ((n->mode & S_IFMT) != S_IFLNK) {
                vnode_put(n);
                return -EINVAL;
            }
            if (c > n->size)
                c = n->size;
            if (!user_range(t->root, b, c, true)) {
                vnode_put(n);
                return -EFAULT;
            }
            r = n->ops->read(n, 0, (void *)b, c);
            vnode_put(n);
            return r;
        }
        case SYS_link: {
            char dest[PATH_MAX], name[NAME_MAX + 1];
            r = path_copy(path, a);
            if (r)
                return r;
            r = path_copy(dest, b);
            if (r)
                return r;
            r = vfs_lookup(path, t->cwd, false, &n);
            if (r)
                return r;
            struct vnode *parent;
            r = vfs_parent(dest, t->cwd, &parent, name);
            if (!r) {
                r = vfs_access(parent, 3);
                if (!r)
                    r = parent->ops->link ? parent->ops->link(parent, name, n) : -EROFS;
                vnode_put(parent);
            }
            vnode_put(n);
            return r;
        }
        case SYS_chmod: {
            r = path_copy(path, a);
            if (r)
                return r;
            r = vfs_lookup(path, t->cwd, true, &n);
            if (r)
                return r;
            if (t->uid && t->uid != n->uid)
                r = -EPERM;
            else
                r = n->ops->chmod ? n->ops->chmod(n, b) : -ENOTSUP;
            vnode_put(n);
            return r;
        }
        case SYS_sysinfo: {
            struct ksysinfo info = {0};
            memory_info(&info);
            info.uptime_ns = monotonic_ns();
            info.realtime_sec = realtime_seconds();
            info.cpus = cpu_count;
            strcpy(info.release, KARIO_VERSION);
            return copy_out(a, &info, sizeof(info));
        }
        case SYS_partscan: {
            if (t->uid)
                return -EPERM;
            r = path_copy(path, a);
            if (r)
                return r;
            const char *name = path;
            if (!strncmp(name, "/dev/", 5))
                name += 5;
            spin_lock(&partition_lock);
            struct blockdev *disk = NULL;
            for (unsigned i = 0; i < block_count; i++)
                if (!strcmp(name, blocks[i].name))
                    disk = &blocks[i];
            if (!disk || disk->parent) {
                spin_unlock(&partition_lock);
                return -EINVAL;
            }
            for (unsigned i = 0; i < block_count; i++)
                if (blocks[i].parent == disk) {
                    spin_unlock(&partition_lock);
                    return -EBUSY;
                }
            r = partitions_scan(disk);
            spin_unlock(&partition_lock);
            return r;
        }
        case SYS_mount: {
            if (t->uid)
                return -EPERM;
            char target[PATH_MAX];
            r = path_copy(path, a);
            if (r)
                return r;
            r = path_copy(target, b);
            if (r)
                return r;
            const char *name = path;
            if (!strncmp(name, "/dev/", 5))
                name += 5;
            struct blockdev *disk = NULL;
            for (unsigned i = 0; i < block_count; i++)
                if (!strcmp(name, blocks[i].name))
                    disk = &blocks[i];
            if (!disk)
                return -ENODEV;
            r = ext2_mount(disk, &n);
            if (r)
                return r;
            r = vfs_mount(target, t->cwd, n);
            vnode_put(n);
            return r;
        }
        default:
            return -ENOSYS;
    }
}
struct frame *syscall_dispatch(struct frame *f)
{
    sched_enter();
    struct task *t = this_cpu()->current;
    if (!t || t->kernel)
        panic("SYSCALL outside user task");
    t->frame = f;
    bool resched = false;
    if (f->rip < PAGE_SIZE || f->rip >= USER_TOP || f->rsp < PAGE_SIZE || f->rsp >= USER_TOP) {
        task_signal_exit(SIGSEGV);
        return schedule(f, false);
    }
    uint64_t nr = f->rax;
    int64_t r = syscall_run(t, f, &resched);
    if ((nr != SYS_execve && nr != SYS_sigreturn) || r)
        f->rax = (uint64_t)r;
    f->flags = (f->flags & 0x240dd5) | 0x202;
    if (resched)
        return schedule(f, false);
    if (!signal_deliver(f))
        return schedule(f, false);
    return f;
}
