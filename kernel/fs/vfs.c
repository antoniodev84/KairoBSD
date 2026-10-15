#include <kernel.h>
struct vnode *vfs_root;
static spinlock_t mount_lock = SPINLOCK_INIT;
int vfs_access(struct vnode *n, unsigned bits)
{
    struct task *t = this_cpu()->current;
    if (!t || !t->uid)
        return (bits & 1) && !S_ISDIR(n->mode) && !(n->mode & 0111) ? -EACCES : 0;
    unsigned shift = t->uid == n->uid ? 6 : t->gid == n->gid ? 3 : 0;
    return ((n->mode >> shift) & bits) == bits ? 0 : -EACCES;
}
void vnode_get(struct vnode *n)
{
    if (n)
        atomic_fetch_add(&n->refs, 1);
}
void vnode_put(struct vnode *n)
{
    if (n && atomic_fetch_sub(&n->refs, 1) == 1) {
        if (n->ops->destroy)
            n->ops->destroy(n);
        kfree(n);
    }
}
static int resolve(const char *path, struct vnode *cwd, bool follow, unsigned depth,
                   struct vnode **out)
{
    if (!path || !*path)
        return -ENOENT;
    if (depth > 40)
        return -ELOOP;
    if (strlen(path) >= PATH_MAX)
        return -ENAMETOOLONG;
    struct vnode *n = path[0] == '/' ? vfs_root : cwd;
    if (!n)
        return -ENOENT;
    vnode_get(n);
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        if (!S_ISDIR(n->mode)) {
            vnode_put(n);
            return -ENOTDIR;
        }
        if (vfs_access(n, 1)) {
            vnode_put(n);
            return -EACCES;
        }
        char name[NAME_MAX + 1];
        size_t len = 0;
        while (*p && *p != '/') {
            if (len == NAME_MAX) {
                vnode_put(n);
                return -ENAMETOOLONG;
            }
            name[len++] = *p++;
        }
        name[len] = 0;
        if (!strcmp(name, "."))
            continue;
        if (!strcmp(name, "..")) {
            struct vnode *parent = n->parent ? n->parent : n;
            vnode_get(parent);
            vnode_put(n);
            n = parent;
            continue;
        }
        struct vnode *next = NULL;
        int r = n->ops->lookup ? n->ops->lookup(n, name, &next) : -ENOTDIR;
        if (r) {
            vnode_put(n);
            return r;
        }
        spin_lock(&mount_lock);
        if (next->mounted) {
            struct vnode *m = next->mounted;
            vnode_get(m);
            vnode_put(next);
            next = m;
        }
        spin_unlock(&mount_lock);
        if ((next->mode & S_IFMT) == S_IFLNK && (follow || *p)) {
            char target[PATH_MAX], combined[PATH_MAX];
            if (next->size >= PATH_MAX || !next->ops->read) {
                vnode_put(next);
                vnode_put(n);
                return -ENAMETOOLONG;
            }
            r = next->ops->read(next, 0, target, next->size);
            if (r < 0) {
                vnode_put(next);
                vnode_put(n);
                return r;
            }
            target[r] = 0;
            if (strlen(target) + strlen(p) >= PATH_MAX) {
                vnode_put(next);
                vnode_put(n);
                return -ENAMETOOLONG;
            }
            strcpy(combined, target);
            strcpy(combined + strlen(target), p);
            vnode_put(next);
            r = resolve(combined, n, follow, depth + 1, out);
            vnode_put(n);
            return r;
        }
        vnode_put(n);
        n = next;
    }
    if (path[strlen(path) - 1] == '/' && !S_ISDIR(n->mode)) {
        vnode_put(n);
        return -ENOTDIR;
    }
    *out = n;
    return 0;
}
int vfs_lookup(const char *p, struct vnode *cwd, bool follow, struct vnode **out)
{
    return resolve(p, cwd, follow, 0, out);
}
int vfs_parent(const char *path, struct vnode *cwd, struct vnode **parent, char *name)
{
    size_t len = strlen(path);
    if (!len)
        return -ENOENT;
    if (len >= PATH_MAX)
        return -ENAMETOOLONG;
    char p[PATH_MAX];
    strcpy(p, path);
    while (len > 1 && p[len - 1] == '/')
        p[--len] = 0;
    char *s = p + len;
    while (s > p && s[-1] != '/')
        s--;
    if (strlen(s) > NAME_MAX || !*s || !strcmp(s, ".") || !strcmp(s, ".."))
        return -EINVAL;
    strcpy(name, s);
    if (s == p) {
        *parent = cwd;
        vnode_get(cwd);
        return S_ISDIR(cwd->mode) ? 0 : -ENOTDIR;
    }
    if (s == p + 1)
        p[1] = 0;
    else
        s[-1] = 0;
    int r = vfs_lookup(p, cwd, true, parent);
    if (!r && !S_ISDIR((*parent)->mode)) {
        vnode_put(*parent);
        return -ENOTDIR;
    }
    return r;
}
int vfs_create(const char *path, struct vnode *cwd, uint32_t mode, const char *target,
               struct vnode **out)
{
    struct vnode *p;
    char name[NAME_MAX + 1];
    int r = vfs_parent(path, cwd, &p, name);
    if (r)
        return r;
    r = vfs_access(p, 3);
    if (!r)
        r = p->ops->create ? p->ops->create(p, name, mode, target, out) : -EROFS;
    if (!r) {
        struct task *t = this_cpu()->current;
        if (t && (*out)->ops->chown) {
            int own = (*out)->ops->chown(*out, t->uid, t->gid);
            if (own) {
                vnode_put(*out);
                r = own;
            }
        }
    }
    vnode_put(p);
    return r;
}
int vfs_remove(const char *path, struct vnode *cwd, bool dir)
{
    struct vnode *p;
    char name[NAME_MAX + 1];
    int r = vfs_parent(path, cwd, &p, name);
    if (r)
        return r;
    r = vfs_access(p, 3);
    if (!r && (p->mode & 01000)) {
        struct task *t = this_cpu()->current;
        struct vnode *victim;
        if (t && t->uid && t->uid != p->uid && !p->ops->lookup(p, name, &victim)) {
            if (t->uid != victim->uid)
                r = -EPERM;
            vnode_put(victim);
        }
    }
    if (!r)
        r = p->ops->remove ? p->ops->remove(p, name, dir) : -EROFS;
    vnode_put(p);
    return r;
}
int vfs_mount(const char *path, struct vnode *cwd, struct vnode *root)
{
    struct vnode *p;
    int r = vfs_lookup(path, cwd, true, &p);
    if (r)
        return r;
    if (!S_ISDIR(p->mode) || !S_ISDIR(root->mode)) {
        vnode_put(p);
        return -ENOTDIR;
    }
    spin_lock(&mount_lock);
    if (p->mounted)
        r = -EBUSY;
    else {
        vnode_get(root);
        p->mounted = root;
        root->parent = p->parent;
    }
    spin_unlock(&mount_lock);
    vnode_put(p);
    return r;
}
int vfs_open(const char *path, struct task *t, unsigned flags, unsigned mode, struct file **out)
{
    if (flags &
            ~(3U | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC) ||
        (flags & 3) == 3)
        return -EINVAL;
    struct vnode *n = NULL;
    int r = vfs_lookup(path, t->cwd, true, &n);
    if (r == -ENOENT && (flags & O_CREAT))
        r = vfs_create(path, t->cwd, S_IFREG | (mode & 0777 & ~t->umask), NULL, &n);
    else if (!r && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        vnode_put(n);
        return -EEXIST;
    }
    if (r)
        return r;
    if ((flags & O_DIRECTORY) && !S_ISDIR(n->mode)) {
        vnode_put(n);
        return -ENOTDIR;
    }
    if (S_ISDIR(n->mode) && (flags & 3) != O_RDONLY) {
        vnode_put(n);
        return -EISDIR;
    }
    unsigned access = ((flags & 3) != O_WRONLY ? 4 : 0) | ((flags & 3) != O_RDONLY ? 2 : 0);
    r = vfs_access(n, access);
    if (r) {
        vnode_put(n);
        return r;
    }
    if (flags & O_TRUNC) {
        if ((flags & 3) == O_RDONLY) {
            vnode_put(n);
            return -EINVAL;
        }
        r = n->ops->truncate ? n->ops->truncate(n, 0) : -ENOTSUP;
        if (r) {
            vnode_put(n);
            return r;
        }
    }
    struct file *f = kzalloc(sizeof(*f));
    if (!f) {
        vnode_put(n);
        return -ENOMEM;
    }
    f->node = n;
    f->flags = flags & (3U | O_NONBLOCK | O_APPEND | O_DIRECTORY);
    atomic_init(&f->refs, 1);
    atomic_flag_clear(&f->lock.flag);
    *out = f;
    return 0;
}
void file_put(struct file *f)
{
    if (f && atomic_fetch_sub(&f->refs, 1) == 1) {
        vnode_put(f->node);
        kfree(f);
    }
}
struct tar_header {
    char name[100], mode[8], uid[8], gid[8], size[12], mtime[12], sum[8], type, link[100], magic[6],
        version[2], uname[32], gname[32], major[8], minor[8], prefix[155], pad[12];
} PACKED;
static bool octal(const char *p, size_t n, uint64_t *out)
{
    uint64_t v = 0;
    bool seen = false;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == 0 || p[i] == ' ') {
            if (seen)
                break;
            continue;
        }
        if (p[i] < '0' || p[i] > '7' || v > (UINT64_MAX - 7) / 8)
            return false;
        v = v * 8 + p[i] - '0';
        seen = true;
    }
    *out = v;
    return true;
}
extern void tmpfs_readonly(struct vnode *);
void vfs_init(const void *data, size_t size)
{
    vfs_root = tmpfs_new(S_IFDIR | 0755);
    if (!vfs_root)
        panic("Root filesystem allocation");
    vfs_root->parent = vfs_root;
    size_t off = 0;
    while (off + 512 <= size) {
        const struct tar_header *h = (const void *)((const uint8_t *)data + off);
        if (!h->name[0])
            break;
        uint64_t len, mode, sum;
        if (memcmp(h->magic, "ustar", 5) || !octal(h->size, 12, &len) ||
            !octal(h->mode, 8, &mode) || !octal(h->sum, 8, &sum))
            panic("Invalid initramfs archive");
        uint64_t actual = 0;
        for (unsigned i = 0; i < 512; i++)
            actual += (i >= 148 && i < 156) ? 32 : ((const uint8_t *)h)[i];
        if (actual != sum || len > size - off - 512 || len > SIZE_MAX - 511)
            panic("Corrupt initramfs entry");
        char path[PATH_MAX], name[101], prefix[156];
        memcpy(name, h->name, 100);
        name[100] = 0;
        memcpy(prefix, h->prefix, 155);
        prefix[155] = 0;
        if (strlen(name) + strlen(prefix) + 3 > PATH_MAX)
            panic("Initramfs path too long");
        if (*prefix)
            snprintf(path, sizeof(path), "/%s/%s", prefix, name);
        else
            snprintf(path, sizeof(path), "/%s", name);
        if (strncmp(path, "/./", 3) == 0)
            memmove(path + 1, path + 3, strlen(path + 3) + 1);
        if (strcmp(path, "/.") && strcmp(path, "/")) {
            struct vnode *n = NULL;
            int r;
            uint32_t type = h->type == '5' ? S_IFDIR : h->type == '2' ? S_IFLNK : S_IFREG;
            char link[101];
            memcpy(link, h->link, 100);
            link[100] = 0;
            r = vfs_create(path, vfs_root, type | (mode & 07777), type == S_IFLNK ? link : NULL,
                           &n);
            if (r == -EEXIST && type == S_IFDIR)
                r = vfs_lookup(path, vfs_root, true, &n);
            if (r)
                panic("Initramfs %s: %d", path, r);
            if (type == S_IFREG && len) {
                r = n->ops->write(n, 0, (const uint8_t *)h + 512, len);
                if (r != (int)len)
                    panic("Initramfs extraction failed");
            }
            vnode_put(n);
        }
        off += 512 + ALIGN_UP(len, 512);
    }
    tmpfs_readonly(vfs_root);
    struct vnode *tmp = tmpfs_new(S_IFDIR | 01777), *dev = devfs_new();
    if (!tmp || !dev || vfs_mount("/tmp", vfs_root, tmp) || vfs_mount("/dev", vfs_root, dev))
        panic("Mounting virtual filesystems");
    vnode_put(tmp);
    vnode_put(dev);
    kprintf("VFS: initramfs root, tmpfs /tmp, devfs /dev mounted\n");
}
