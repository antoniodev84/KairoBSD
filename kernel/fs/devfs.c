#include <kernel.h>
static struct vnode_ops ops;
struct devfs_view {
    struct vnode *root;
    struct devfs_view *next;
};
static struct devfs_view *views;
static spinlock_t views_lock = SPINLOCK_INIT;
bool devfs_is_console(struct vnode *node)
{
    return node && node->ops == &ops && (uintptr_t)node->data == 1;
}
static int read_dev(struct vnode *n, uint64_t off, void *p, size_t len)
{
    uintptr_t id = (uintptr_t)n->data;
    if (id == 1) {
        if (!len)
            return 0;
        struct task *task = this_cpu()->current;
        int foreground = atomic_load(&terminal_pgid);
        if (task && foreground && task->pgid != foreground) {
            spin_lock(&sched_lock);
            bool blocked =
                (task->signal_mask & (1UL << (SIGTTIN - 1))) || task->actions[SIGTTIN].handler == 1;
            spin_unlock(&sched_lock);
            if (blocked)
                return -EIO;
            signal_queue_self(SIGTTIN);
            return -EAGAIN;
        }
        int c = serial_get();
        if (c < 0)
            return -EAGAIN;
        ((char *)p)[0] = (char)c;
        return 1;
    }
    if (id == 2)
        return 0;
    if (id == 3) {
        memset(p, 0, len);
        return (int)len;
    }
    if (id >= 16) {
        struct blockdev *b = &blocks[id - 16];
        if (off >= b->sectors * 512)
            return 0;
        if (len > b->sectors * 512 - off)
            len = b->sectors * 512 - off;
        uint8_t buf[512];
        size_t done = 0;
        spin_lock(&n->lock);
        while (done < len) {
            uint64_t at = off + done;
            size_t k = 512 - (at % 512);
            if (k > len - done)
                k = len - done;
            int r = block_io(b, at / 512, 1, buf, false);
            if (r) {
                spin_unlock(&n->lock);
                return done ? (int)done : r;
            }
            memcpy((uint8_t *)p + done, buf + at % 512, k);
            done += k;
        }
        spin_unlock(&n->lock);
        return (int)done;
    }
    return -ENODEV;
}
static int write_dev(struct vnode *n, uint64_t off, const void *p, size_t len)
{
    uintptr_t id = (uintptr_t)n->data;
    if (id == 1) {
        console_write(p, len);
        return (int)len;
    }
    if (id == 2 || id == 3)
        return (int)len;
    if (id >= 16) {
        struct blockdev *b = &blocks[id - 16];
        if (off > b->sectors * 512 || len > b->sectors * 512 - off)
            return -ENOSPC;
        uint8_t buf[512];
        size_t done = 0;
        spin_lock(&n->lock);
        while (done < len) {
            uint64_t at = off + done;
            size_t k = 512 - (at % 512);
            if (k > len - done)
                k = len - done;
            int r = 0;
            if (k != 512)
                r = block_io(b, at / 512, 1, buf, false);
            if (!r) {
                memcpy(buf + at % 512, (const uint8_t *)p + done, k);
                r = block_io(b, at / 512, 1, buf, true);
            }
            if (r) {
                spin_unlock(&n->lock);
                return done ? (int)done : r;
            }
            done += k;
        }
        spin_unlock(&n->lock);
        return (int)done;
    }
    return -ENODEV;
}
static struct vnode_ops ops = {.read = read_dev, .write = write_dev};
struct vnode *devfs_new(void)
{
    struct vnode *root = tmpfs_new(S_IFDIR | 0755);
    if (!root)
        return NULL;
    const char *names[] = {"console", "null", "zero"};
    for (unsigned i = 0; i < 3 + block_count; i++) {
        struct vnode *n;
        const char *name = i < 3 ? names[i] : blocks[i - 3].name;
        int r = root->ops->create(root, name, (i < 3 ? S_IFCHR : S_IFBLK) | 0600, NULL, &n);
        if (r) {
            vnode_put(root);
            return NULL;
        }
        n->ops->destroy(n);
        n->ops = &ops;
        n->data = (void *)(uintptr_t)(i < 3 ? i + 1 : i - 3 + 16);
        n->fs = NULL;
        if (i >= 3)
            n->size = blocks[i - 3].sectors * 512;
        vnode_put(n);
    }
    struct devfs_view *view = kmalloc(sizeof(*view));
    if (!view) {
        vnode_put(root);
        return NULL;
    }
    view->root = root;
    vnode_get(root);
    spin_lock(&views_lock);
    view->next = views;
    views = view;
    spin_unlock(&views_lock);
    return root;
}

void devfs_forget(struct vnode *root)
{
    spin_lock(&views_lock);
    struct devfs_view **link = &views;
    while (*link && (*link)->root != root)
        link = &(*link)->next;
    struct devfs_view *view = *link;
    if (view)
        *link = view->next;
    spin_unlock(&views_lock);
    if (view) {
        vnode_put(view->root);
        kfree(view);
    }
}
int devfs_register_block(unsigned index)
{
    if (index >= 32)
        return -EINVAL;
    int result = 0;
    spin_lock(&views_lock);
    for (struct devfs_view *view = views; view; view = view->next) {
        struct vnode *node;
        result =
            view->root->ops->create(view->root, blocks[index].name, S_IFBLK | 0600, NULL, &node);
        if (result)
            break;
        node->ops->destroy(node);
        node->ops = &ops;
        node->data = (void *)(uintptr_t)(index + 16);
        node->fs = NULL;
        node->size = blocks[index].sectors * 512;
        vnode_put(node);
    }
    if (result) {
        for (struct devfs_view *view = views; view; view = view->next) {
            struct vnode *node;
            if (view->root->ops->lookup(view->root, blocks[index].name, &node))
                continue;
            bool created = node->ops == &ops && (uintptr_t)node->data == index + 16;
            vnode_put(node);
            if (created) {
                int cleanup = view->root->ops->remove(view->root, blocks[index].name, false);
                if (cleanup)
                    kprintf("devfs block registration cleanup failed: %d\n", cleanup);
            }
        }
    }
    spin_unlock(&views_lock);
    return result;
}
