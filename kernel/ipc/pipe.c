#include <kernel.h>
struct pipe_buffer {
    spinlock_t lock;
    uint8_t bytes[PIPE_BUF];
    size_t head, used;
    unsigned endpoints, readers, writers;
    uint64_t inode;
};
struct pipe_end {
    struct pipe_buffer *pipe;
    bool writer;
};
static atomic_uint_fast64_t next_inode = 1;
static int read_pipe(struct vnode *n, uint64_t offset, void *data, size_t length)
{
    struct pipe_buffer *p = ((struct pipe_end *)n->data)->pipe;
    spin_lock(&p->lock);
    if (!p->used) {
        int result = p->writers ? -EAGAIN : 0;
        spin_unlock(&p->lock);
        return result;
    }
    if (length > p->used)
        length = p->used;
    for (size_t i = 0; i < length; i++)
        ((uint8_t *)data)[i] = p->bytes[(p->head + i) % PIPE_BUF];
    p->head = (p->head + length) % PIPE_BUF;
    p->used -= length;
    spin_unlock(&p->lock);
    return (int)length;
}
static int write_pipe(struct vnode *n, uint64_t offset, const void *data, size_t length)
{
    struct pipe_buffer *p = ((struct pipe_end *)n->data)->pipe;
    spin_lock(&p->lock);
    if (!p->readers) {
        spin_unlock(&p->lock);
        return -EPIPE;
    }
    size_t room = PIPE_BUF - p->used;
    if (!room || (length <= PIPE_BUF && length > room)) {
        spin_unlock(&p->lock);
        return -EAGAIN;
    }
    if (length > room)
        length = room;
    for (size_t i = 0; i < length; i++)
        p->bytes[(p->head + p->used + i) % PIPE_BUF] = ((const uint8_t *)data)[i];
    p->used += length;
    spin_unlock(&p->lock);
    return (int)length;
}
static void destroy_pipe(struct vnode *n)
{
    struct pipe_end *end = n->data;
    struct pipe_buffer *p = end->pipe;
    spin_lock(&p->lock);
    if (end->writer)
        p->writers--;
    else
        p->readers--;
    bool last = !--p->endpoints;
    spin_unlock(&p->lock);
    kfree(end);
    if (last)
        kfree(p);
}
static struct vnode_ops pipe_ops = {
    .read = read_pipe, .write = write_pipe, .destroy = destroy_pipe};
int pipe_create(struct task *t, unsigned flags, int *fds)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK))
        return -EINVAL;
    unsigned found = 0;
    for (unsigned i = 0; i < MAX_FDS && found < 2; i++)
        if (!t->fds[i])
            fds[found++] = i;
    if (found != 2)
        return -EMFILE;
    struct pipe_buffer *p = kzalloc(sizeof(*p));
    if (!p)
        return -ENOMEM;
    atomic_flag_clear(&p->lock.flag);
    p->inode = atomic_fetch_add(&next_inode, 1);
    struct file *files[2] = {NULL, NULL};
    for (unsigned i = 0; i < 2; i++) {
        struct file *f = kzalloc(sizeof(*f));
        struct vnode *n = kzalloc(sizeof(*n));
        struct pipe_end *end = kzalloc(sizeof(*end));
        if (!f || !n || !end) {
            kfree(f);
            kfree(n);
            kfree(end);
            if (files[0])
                file_put(files[0]);
            else
                kfree(p);
            return -ENOMEM;
        }
        end->pipe = p;
        end->writer = i;
        p->endpoints++;
        if (i)
            p->writers++;
        else
            p->readers++;
        n->ino = p->inode;
        n->nlink = 1;
        n->mode = S_IFIFO | 0600;
        n->uid = t->uid;
        n->gid = t->gid;
        n->ops = &pipe_ops;
        n->data = end;
        atomic_init(&n->refs, 1);
        atomic_flag_clear(&n->lock.flag);
        f->node = n;
        f->flags = (i ? O_WRONLY : O_RDONLY) | (flags & O_NONBLOCK);
        atomic_init(&f->refs, 1);
        atomic_flag_clear(&f->lock.flag);
        files[i] = f;
    }
    for (unsigned i = 0; i < 2; i++) {
        t->fds[fds[i]] = files[i];
        t->fd_flags[fds[i]] = flags & O_CLOEXEC ? FD_CLOEXEC : 0;
    }
    return 0;
}
