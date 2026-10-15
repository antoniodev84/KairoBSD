#include <kernel.h>
struct dent {
    char name[NAME_MAX + 1];
    struct vnode *node;
    struct dent *next;
};
struct memnode {
    struct dent *entries;
    uint8_t *bytes;
    size_t capacity;
};
struct memfs {
    bool readonly;
    atomic_uint nodes;
    spinlock_t lock;
};
static atomic_uint_fast64_t next_ino = 1;
static struct vnode_ops ops;
static struct vnode *new_node(struct memfs *fs, uint32_t mode)
{
    struct vnode *n = kzalloc(sizeof(*n));
    struct memnode *m = kzalloc(sizeof(*m));
    if (!n || !m) {
        kfree(n);
        kfree(m);
        return NULL;
    }
    n->ino = atomic_fetch_add(&next_ino, 1);
    n->mode = mode;
    n->nlink = S_ISDIR(mode) ? 2 : 1;
    n->atime = n->mtime = n->ctime = realtime_seconds();
    n->ops = &ops;
    n->data = m;
    n->fs = fs;
    atomic_init(&n->refs, 1);
    atomic_fetch_add(&fs->nodes, 1);
    atomic_flag_clear(&n->lock.flag);
    return n;
}
struct vnode *tmpfs_new(uint32_t mode)
{
    struct memfs *fs = kzalloc(sizeof(*fs));
    if (!fs)
        return NULL;
    atomic_init(&fs->nodes, 0);
    atomic_flag_clear(&fs->lock.flag);
    struct vnode *n = new_node(fs, mode);
    if (!n)
        kfree(fs);
    return n;
}
void tmpfs_readonly(struct vnode *n)
{
    ((struct memfs *)n->fs)->readonly = true;
}
static int lookup(struct vnode *n, const char *s, struct vnode **out)
{
    spin_lock(&((struct memfs *)n->fs)->lock);
    struct memnode *m = n->data;
    for (struct dent *d = m->entries; d; d = d->next)
        if (!strcmp(d->name, s)) {
            vnode_get(d->node);
            *out = d->node;
            spin_unlock(&((struct memfs *)n->fs)->lock);
            return 0;
        }
    spin_unlock(&((struct memfs *)n->fs)->lock);
    return -ENOENT;
}
static int read_node(struct vnode *n, uint64_t off, void *p, size_t len)
{
    if (S_ISDIR(n->mode))
        return -EISDIR;
    spin_lock(&((struct memfs *)n->fs)->lock);
    if (off >= n->size)
        len = 0;
    else if (len > n->size - off)
        len = n->size - off;
    if (len)
        memcpy(p, ((struct memnode *)n->data)->bytes + off, len);
    spin_unlock(&((struct memfs *)n->fs)->lock);
    return (int)len;
}
static int capacity(struct memnode *m, size_t need)
{
    if (need <= m->capacity)
        return 0;
    if (need > SIZE_MAX - 4095)
        return -EFBIG;
    size_t cap = ALIGN_UP(need, 4096);
    void *p = krealloc(m->bytes, cap);
    if (!p)
        return -ENOMEM;
    memset((char *)p + m->capacity, 0, cap - m->capacity);
    m->bytes = p;
    m->capacity = cap;
    return 0;
}
static int write_node(struct vnode *n, uint64_t off, const void *p, size_t len)
{
    if (((struct memfs *)n->fs)->readonly)
        return -EROFS;
    if (S_ISDIR(n->mode))
        return -EISDIR;
    if (len > INT32_MAX || off > SIZE_MAX - len)
        return -EFBIG;
    spin_lock(&((struct memfs *)n->fs)->lock);
    struct memnode *m = n->data;
    int r = capacity(m, off + len);
    if (!r) {
        memcpy(m->bytes + off, p, len);
        if (off + len > n->size)
            n->size = off + len;
        n->blocks = m->capacity / 512;
        n->mtime = n->ctime = realtime_seconds();
    }
    spin_unlock(&((struct memfs *)n->fs)->lock);
    return r ? r : (int)len;
}
static int truncate_node(struct vnode *n, uint64_t size)
{
    if (((struct memfs *)n->fs)->readonly)
        return -EROFS;
    if (S_ISDIR(n->mode))
        return -EISDIR;
    spin_lock(&((struct memfs *)n->fs)->lock);
    struct memnode *m = n->data;
    int r = capacity(m, size);
    if (!r) {
        if (size < n->size)
            memset(m->bytes + size, 0, n->size - size);
        n->size = size;
        n->blocks = m->capacity / 512;
        n->mtime = n->ctime = realtime_seconds();
    }
    spin_unlock(&((struct memfs *)n->fs)->lock);
    return r;
}
static int create(struct vnode *p, const char *name, uint32_t mode, const char *target,
                  struct vnode **out)
{
    if (((struct memfs *)p->fs)->readonly)
        return -EROFS;
    struct dent *d = kzalloc(sizeof(*d));
    struct vnode *n = new_node(p->fs, mode);
    if (!d || !n) {
        kfree(d);
        vnode_put(n);
        return -ENOMEM;
    }
    strcpy(d->name, name);
    d->node = n;
    n->parent = p;
    if ((mode & S_IFMT) == S_IFLNK) {
        int r = write_node(n, 0, target, strlen(target));
        if (r < 0) {
            vnode_put(n);
            kfree(d);
            return r;
        }
    }
    spin_lock(&((struct memfs *)p->fs)->lock);
    struct memnode *m = p->data;
    for (struct dent *e = m->entries; e; e = e->next)
        if (!strcmp(e->name, name)) {
            spin_unlock(&((struct memfs *)p->fs)->lock);
            vnode_put(n);
            kfree(d);
            return -EEXIST;
        }
    d->next = m->entries;
    m->entries = d;
    if (S_ISDIR(mode))
        p->nlink++;
    p->mtime = p->ctime = realtime_seconds();
    vnode_get(n);
    *out = n;
    spin_unlock(&((struct memfs *)p->fs)->lock);
    return 0;
}
static int remove_node(struct vnode *p, const char *name, bool dir)
{
    if (((struct memfs *)p->fs)->readonly)
        return -EROFS;
    spin_lock(&((struct memfs *)p->fs)->lock);
    struct memnode *m = p->data;
    struct dent **link = &m->entries;
    while (*link && strcmp((*link)->name, name))
        link = &(*link)->next;
    if (!*link) {
        spin_unlock(&((struct memfs *)p->fs)->lock);
        return -ENOENT;
    }
    struct dent *d = *link;
    struct vnode *n = d->node;
    if (dir != S_ISDIR(n->mode)) {
        spin_unlock(&((struct memfs *)p->fs)->lock);
        return dir ? -ENOTDIR : -EISDIR;
    }
    if (n->mounted || (dir && atomic_load(&n->refs) > 1)) {
        spin_unlock(&((struct memfs *)p->fs)->lock);
        return -EBUSY;
    }
    if (dir && ((struct memnode *)n->data)->entries) {
        spin_unlock(&((struct memfs *)p->fs)->lock);
        return -ENOTEMPTY;
    }
    *link = d->next;
    if (dir) {
        n->nlink = 0;
        p->nlink--;
    } else
        n->nlink--;
    p->mtime = p->ctime = realtime_seconds();
    spin_unlock(&((struct memfs *)p->fs)->lock);
    vnode_put(n);
    kfree(d);
    return 0;
}
static int link_node(struct vnode *p, const char *name, struct vnode *n)
{
    if (p->fs != n->fs)
        return -EXDEV;
    if (S_ISDIR(n->mode))
        return -EPERM;
    if (((struct memfs *)p->fs)->readonly)
        return -EROFS;
    struct dent *d = kzalloc(sizeof(*d));
    if (!d)
        return -ENOMEM;
    strcpy(d->name, name);
    d->node = n;
    spin_lock(&((struct memfs *)p->fs)->lock);
    struct memnode *m = p->data;
    for (struct dent *e = m->entries; e; e = e->next)
        if (!strcmp(e->name, name)) {
            spin_unlock(&((struct memfs *)p->fs)->lock);
            kfree(d);
            return -EEXIST;
        }
    if (n->nlink == UINT32_MAX) {
        spin_unlock(&((struct memfs *)p->fs)->lock);
        kfree(d);
        return -EMLINK;
    }
    vnode_get(n);
    n->nlink++;
    d->next = m->entries;
    m->entries = d;
    spin_unlock(&((struct memfs *)p->fs)->lock);
    return 0;
}
static int readdir_node(struct vnode *n, uint64_t index, struct kdirent *out)
{
    spin_lock(&((struct memfs *)n->fs)->lock);
    struct dent *d = ((struct memnode *)n->data)->entries;
    while (d && index--)
        d = d->next;
    if (!d) {
        spin_unlock(&((struct memfs *)n->fs)->lock);
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->ino = d->node->ino;
    out->mode = d->node->mode;
    strcpy(out->name, d->name);
    spin_unlock(&((struct memfs *)n->fs)->lock);
    return 1;
}
static int chmod_node(struct vnode *n, uint32_t m)
{
    if (((struct memfs *)n->fs)->readonly)
        return -EROFS;
    spin_lock(&((struct memfs *)n->fs)->lock);
    n->mode = (n->mode & S_IFMT) | (m & 07777);
    n->ctime = realtime_seconds();
    spin_unlock(&((struct memfs *)n->fs)->lock);
    return 0;
}
static int chown_node(struct vnode *n, uint32_t uid, uint32_t gid)
{
    if (((struct memfs *)n->fs)->readonly)
        return -EROFS;
    spin_lock(&((struct memfs *)n->fs)->lock);
    if (uid != UINT32_MAX)
        n->uid = uid;
    if (gid != UINT32_MAX)
        n->gid = gid;
    n->mode &= ~06000U;
    spin_unlock(&((struct memfs *)n->fs)->lock);
    return 0;
}
static void destroy(struct vnode *n)
{
    struct memnode *m = n->data;
    while (m->entries) {
        struct dent *d = m->entries;
        m->entries = d->next;
        vnode_put(d->node);
        kfree(d);
    }
    kfree(m->bytes);
    kfree(m);
    struct memfs *fs = n->fs;
    if (atomic_fetch_sub(&fs->nodes, 1) == 1)
        kfree(fs);
}
static struct vnode_ops ops = {read_node,   write_node, lookup,       create,
                               remove_node, link_node,  readdir_node, truncate_node,
                               chmod_node,  chown_node, destroy};
