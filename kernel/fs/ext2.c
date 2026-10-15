#include <kernel.h>
struct ext_super {
    uint32_t inodes, blocks, reserved_blocks, free_blocks, free_inodes, first_data, log_block,
        log_frag, blocks_group, frags_group, inodes_group, mtime, wtime;
    uint16_t mounts, max_mounts, magic, state, errors, minor;
    uint32_t lastcheck, checkinterval, creator, revision;
    uint16_t default_uid, default_gid;
    uint32_t first_inode;
    uint16_t inode_size, group_nr;
    uint32_t compat, incompat, rocompat;
    uint8_t rest[1024 - 104];
} PACKED;
struct ext_group {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t free_blocks, free_inodes, used_dirs, pad;
    uint32_t reserved[3];
} PACKED;
struct ext_inode {
    uint16_t mode, uid;
    uint32_t size, atime, ctime, mtime, dtime;
    uint16_t gid, links;
    uint32_t sectors, flags, osd1, block[15], generation, file_acl, size_high, faddr;
    uint8_t osd2[12];
} PACKED;
struct ext_dir {
    uint32_t inode;
    uint16_t length;
    uint8_t name_len, type;
} PACKED;
struct ext_node {
    struct ext_inode inode;
    struct vnode *vnode;
    struct ext_node *next;
    bool parent_ref;
};
struct ext_fs {
    struct blockdev *disk;
    struct ext_super super;
    struct ext_group *groups;
    uint32_t group_count, block_size, gdt_start;
    spinlock_t lock;
    struct ext_node *nodes;
    unsigned node_count;
    struct ext_fs *next;
    bool failed;
};
static struct vnode_ops ops;
static struct ext_fs *mounted_filesystems;
static spinlock_t filesystems_lock = SPINLOCK_INIT;
static uint32_t now_seconds(void)
{
    return (uint32_t)realtime_seconds();
}
static int disk_bytes(struct ext_fs *f, uint64_t off, void *p, size_t len, bool write)
{
    uint8_t buf[512];
    size_t done = 0;
    if (off > f->disk->sectors * 512 || len > f->disk->sectors * 512 - off)
        return -EIO;
    while (done < len) {
        uint64_t at = off + done;
        size_t n = 512 - at % 512;
        if (n > len - done)
            n = len - done;
        int r = 0;
        if (!write || n != 512)
            r = block_io(f->disk, at / 512, 1, buf, false);
        if (!r) {
            if (write) {
                memcpy(buf + at % 512, (uint8_t *)p + done, n);
                r = block_io(f->disk, at / 512, 1, buf, true);
            } else
                memcpy((uint8_t *)p + done, buf + at % 512, n);
        }
        if (r) {
            if (write)
                f->failed = true;
            return r;
        }
        done += n;
    }
    return 0;
}
static int flush_disk(struct ext_fs *f)
{
    struct blockdev *b = f->disk;
    while (b->parent)
        b = b->parent;
    if (!b->flush)
        return -ENOTSUP;
    return b->flush(b);
}
static int mark_dirty(struct ext_fs *f)
{
    if (f->failed)
        return -EROFS;
    if (f->super.state == 2)
        return 0;
    f->super.state = 2;
    int r = disk_bytes(f, 1024, &f->super, 1024, true);
    if (!r)
        r = flush_disk(f);
    if (r)
        f->failed = true;
    return r;
}
static int bread(struct ext_fs *f, uint32_t block, void *p)
{
    if (block >= f->super.blocks)
        return -EIO;
    return block_io(f->disk, (uint64_t)block * (f->block_size / 512), f->block_size / 512, p,
                    false);
}
static int bwrite(struct ext_fs *f, uint32_t block, void *p)
{
    if (f->failed)
        return -EROFS;
    if (block >= f->super.blocks)
        return -EIO;
    int r = mark_dirty(f);
    if (!r)
        r = block_io(f->disk, (uint64_t)block * (f->block_size / 512), f->block_size / 512, p,
                     true);
    if (r)
        f->failed = true;
    return r;
}
static int sync_meta(struct ext_fs *f)
{
    if (f->failed)
        return -EROFS;
    f->super.wtime = now_seconds();
    int r = disk_bytes(f, 1024, &f->super, 1024, true);
    if (!r)
        r = disk_bytes(f, (uint64_t)f->gdt_start * f->block_size, f->groups,
                       f->group_count * sizeof(struct ext_group), true);
    if (!r)
        r = flush_disk(f);
    if (!r) {
        f->super.state = 1;
        r = disk_bytes(f, 1024, &f->super, 1024, true);
        if (!r)
            r = flush_disk(f);
    }
    if (r)
        f->failed = true;
    return r;
}
static void mutation_failed(struct ext_fs *f, int result)
{
    if (result < 0 && f->super.state == 2 && !f->failed) {
        f->failed = true;
        kprintf("ext2: %s mutation failed %d; filesystem frozen read-only\n", f->disk->name,
                result);
    }
}
static uint64_t inode_offset(struct ext_fs *f, uint32_t ino)
{
    unsigned g = (ino - 1) / f->super.inodes_group, i = (ino - 1) % f->super.inodes_group;
    return (uint64_t)f->groups[g].inode_table * f->block_size + (uint64_t)i * f->super.inode_size;
}
static int inode_read(struct ext_fs *f, uint32_t ino, struct ext_inode *in)
{
    if (!ino || ino > f->super.inodes)
        return -EIO;
    return disk_bytes(f, inode_offset(f, ino), in, sizeof(*in), false);
}
static int inode_write(struct ext_fs *f, uint32_t ino, struct ext_inode *in)
{
    if (f->failed)
        return -EROFS;
    int r = mark_dirty(f);
    return r ? r : disk_bytes(f, inode_offset(f, ino), in, sizeof(*in), true);
}
static void refresh(struct vnode *n)
{
    struct ext_inode *i = &((struct ext_node *)n->data)->inode;
    n->mode = i->mode;
    n->uid = i->uid | ((uint32_t)i->osd2[4] << 16) | ((uint32_t)i->osd2[5] << 24);
    n->gid = i->gid | ((uint32_t)i->osd2[6] << 16) | ((uint32_t)i->osd2[7] << 24);
    n->nlink = i->links;
    n->blocks = i->sectors;
    n->atime = i->atime;
    n->mtime = i->mtime;
    n->ctime = i->ctime;
    n->size = i->size;
    if ((i->mode & S_IFMT) == S_IFREG)
        n->size |= (uint64_t)i->size_high << 32;
}
static struct vnode *iget(struct ext_fs *f, uint32_t ino)
{
    for (struct ext_node *e = f->nodes; e; e = e->next)
        if (e->vnode->ino == ino) {
            unsigned refs = atomic_load(&e->vnode->refs);
            while (refs && !atomic_compare_exchange_weak(&e->vnode->refs, &refs, refs + 1)) {
            }
            if (refs)
                return e->vnode;
        }
    struct ext_node *e = kzalloc(sizeof(*e));
    struct vnode *n = kzalloc(sizeof(*n));
    if (!e || !n) {
        kfree(e);
        kfree(n);
        return NULL;
    }
    if (inode_read(f, ino, &e->inode) || !e->inode.mode) {
        kfree(e);
        kfree(n);
        return NULL;
    }
    f->node_count++;
    e->vnode = n;
    e->next = f->nodes;
    f->nodes = e;
    n->ino = ino;
    n->fs = f;
    n->data = e;
    n->ops = &ops;
    atomic_init(&n->refs, 1);
    atomic_flag_clear(&n->lock.flag);
    refresh(n);
    return n;
}
static int alloc_bit(struct ext_fs *f, bool inode, unsigned preferred, uint32_t *out)
{
    if (f->failed)
        return -EROFS;
    uint8_t *buf = kmalloc(f->block_size);
    if (!buf)
        return -ENOMEM;
    int result = -ENOSPC;
    for (unsigned k = 0; k < f->group_count; k++) {
        unsigned g = (preferred + k) % f->group_count;
        struct ext_group *d = &f->groups[g];
        if (!(inode ? d->free_inodes : d->free_blocks))
            continue;
        uint32_t map = inode ? d->inode_bitmap : d->block_bitmap;
        int r = bread(f, map, buf);
        if (r) {
            result = r;
            break;
        }
        uint32_t base = inode ? g * f->super.inodes_group + 1
                              : g * f->super.blocks_group + f->super.first_data,
                 count = inode ? f->super.inodes_group : f->super.blocks_group,
                 limit = inode ? f->super.inodes + 1 : f->super.blocks;
        if (base >= limit)
            continue;
        if (count > limit - base)
            count = limit - base;
        for (uint32_t i = 0; i < count; i++)
            if (!(buf[i / 8] & (1U << (i % 8))) && (!inode || base + i >= f->super.first_inode)) {
                buf[i / 8] |= 1U << (i % 8);
                r = bwrite(f, map, buf);
                if (r) {
                    result = r;
                    goto done;
                }
                if (inode) {
                    d->free_inodes--;
                    f->super.free_inodes--;
                } else {
                    d->free_blocks--;
                    f->super.free_blocks--;
                }
                *out = base + i;
                result = 0;
                goto done;
            }
    }
done:
    kfree(buf);
    return result;
}
static int free_bit(struct ext_fs *f, bool inode, uint32_t number)
{
    uint32_t first = inode ? 1 : f->super.first_data,
             limit = inode ? f->super.inodes + 1 : f->super.blocks,
             per = inode ? f->super.inodes_group : f->super.blocks_group;
    if (number < first || number >= limit)
        return -EIO;
    unsigned g = (number - first) / per, i = (number - first) % per;
    uint8_t *buf = kmalloc(f->block_size);
    if (!buf)
        return -ENOMEM;
    uint32_t map = inode ? f->groups[g].inode_bitmap : f->groups[g].block_bitmap;
    int r = bread(f, map, buf);
    if (!r) {
        if (!(buf[i / 8] & (1U << (i % 8))))
            r = -EIO;
        else {
            buf[i / 8] &= ~(1U << (i % 8));
            r = bwrite(f, map, buf);
            if (!r) {
                if (inode) {
                    f->groups[g].free_inodes++;
                    f->super.free_inodes++;
                } else {
                    f->groups[g].free_blocks++;
                    f->super.free_blocks++;
                }
            }
        }
    }
    kfree(buf);
    return r;
}
static int new_block(struct ext_fs *f, unsigned group, struct ext_inode *in, uint32_t *out)
{
    int r = alloc_bit(f, false, group, out);
    if (r)
        return r;
    void *zero = kzalloc(f->block_size);
    if (!zero) {
        free_bit(f, false, *out);
        return -ENOMEM;
    }
    r = bwrite(f, *out, zero);
    kfree(zero);
    if (r) {
        free_bit(f, false, *out);
        return r;
    }
    if (in->sectors > UINT32_MAX - f->block_size / 512) {
        free_bit(f, false, *out);
        return -EFBIG;
    }
    in->sectors += f->block_size / 512;
    return 0;
}
static int data_block(struct vnode *n, uint64_t index, bool create, uint32_t *out)
{
    struct ext_fs *f = n->fs;
    struct ext_inode *in = &((struct ext_node *)n->data)->inode;
    unsigned group = (n->ino - 1) / f->super.inodes_group;
    uint64_t per = f->block_size / 4;
    unsigned depth;
    uint32_t root, indices[3];
    if (index < 12) {
        if (!in->block[index] && create) {
            int r = new_block(f, group, in, &in->block[index]);
            if (r)
                return r;
            r = inode_write(f, n->ino, in);
            if (r)
                return r;
        }
        *out = in->block[index];
        return 0;
    }
    index -= 12;
    if (index < per) {
        depth = 1;
        root = 12;
        indices[0] = index;
    } else if ((index -= per) < per * per) {
        depth = 2;
        root = 13;
        indices[0] = index / per;
        indices[1] = index % per;
    } else if ((index -= per * per) < per * per * per) {
        depth = 3;
        root = 14;
        indices[0] = index / (per * per);
        indices[1] = (index / per) % per;
        indices[2] = index % per;
    } else
        return -EFBIG;
    if (!in->block[root]) {
        if (!create) {
            *out = 0;
            return 0;
        }
        int r = new_block(f, group, in, &in->block[root]);
        if (r)
            return r;
        r = inode_write(f, n->ino, in);
        if (r)
            return r;
    }
    uint32_t b = in->block[root];
    uint32_t *table = kmalloc(f->block_size);
    if (!table)
        return -ENOMEM;
    int r = 0;
    for (unsigned level = 0; level < depth; level++) {
        r = bread(f, b, table);
        if (r)
            break;
        uint32_t next = table[indices[level]];
        if (!next && create) {
            r = new_block(f, group, in, &next);
            if (r)
                break;
            table[indices[level]] = next;
            r = bwrite(f, b, table);
            if (r)
                break;
            r = inode_write(f, n->ino, in);
            if (r)
                break;
        }
        if (!next) {
            b = 0;
            break;
        }
        b = next;
    }
    kfree(table);
    if (!r)
        *out = b;
    return r;
}
static int read_locked(struct vnode *n, uint64_t off, void *p, size_t len)
{
    if (off >= n->size)
        return 0;
    if (len > n->size - off)
        len = n->size - off;
    struct ext_fs *f = n->fs;
    struct ext_inode *in = &((struct ext_node *)n->data)->inode;
    if ((n->mode & S_IFMT) == S_IFLNK && n->size <= 60 && !in->sectors) {
        memcpy(p, (uint8_t *)in->block + off, len);
        return (int)len;
    }
    uint8_t *buf = kmalloc(f->block_size);
    if (!buf)
        return -ENOMEM;
    size_t done = 0;
    int r = 0;
    while (done < len) {
        uint64_t at = off + done;
        size_t k = f->block_size - at % f->block_size;
        if (k > len - done)
            k = len - done;
        uint32_t b;
        r = data_block(n, at / f->block_size, false, &b);
        if (r)
            break;
        if (b)
            r = bread(f, b, buf);
        else
            memset(buf, 0, f->block_size);
        if (r)
            break;
        memcpy((uint8_t *)p + done, buf + at % f->block_size, k);
        done += k;
    }
    kfree(buf);
    return done ? (int)done : r;
}
static int write_locked(struct vnode *n, uint64_t off, const void *p, size_t len)
{
    struct ext_fs *f = n->fs;
    struct ext_inode *in = &((struct ext_node *)n->data)->inode;
    if (f->failed)
        return -EROFS;
    if (in->flags & 0x10)
        return -EPERM;
    if ((in->flags & 0x20) && off != n->size)
        return -EPERM;
    if (off > UINT64_MAX - len || len > INT32_MAX)
        return -EFBIG;
    uint8_t *buf = kmalloc(f->block_size);
    if (!buf)
        return -ENOMEM;
    size_t done = 0;
    int r = 0;
    while (done < len) {
        uint64_t at = off + done;
        size_t k = f->block_size - at % f->block_size;
        if (k > len - done)
            k = len - done;
        uint32_t b;
        r = data_block(n, at / f->block_size, true, &b);
        if (r)
            break;
        r = bread(f, b, buf);
        if (r)
            break;
        memcpy(buf + at % f->block_size, (const uint8_t *)p + done, k);
        r = bwrite(f, b, buf);
        if (r)
            break;
        done += k;
    }
    if (done) {
        if (off + done > n->size)
            n->size = off + done;
        in->size = n->size;
        if ((n->mode & S_IFMT) == S_IFREG)
            in->size_high = n->size >> 32;
        in->mtime = in->ctime = now_seconds();
        int w = inode_write(f, n->ino, in);
        if (w)
            r = w;
    }
    refresh(n);
    kfree(buf);
    return r ? r : (int)done;
}
static int read_node(struct vnode *n, uint64_t off, void *p, size_t len)
{
    if ((n->mode & S_IFMT) != S_IFREG && (n->mode & S_IFMT) != S_IFLNK && !S_ISDIR(n->mode))
        return -ENXIO;
    if (S_ISDIR(n->mode))
        return -EISDIR;
    struct ext_fs *f = n->fs;
    spin_lock(&f->lock);
    int r = read_locked(n, off, p, len);
    spin_unlock(&f->lock);
    return r;
}
static int write_node(struct vnode *n, uint64_t off, const void *p, size_t len)
{
    if (S_ISDIR(n->mode))
        return -EISDIR;
    if ((n->mode & S_IFMT) != S_IFREG)
        return -EINVAL;
    struct ext_fs *f = n->fs;
    spin_lock(&f->lock);
    int r = write_locked(n, off, p, len);
    if (r >= 0) {
        int s = sync_meta(f);
        if (s)
            r = s;
    }
    mutation_failed(f, r);
    spin_unlock(&f->lock);
    return r;
}
static int dir_find(struct vnode *n, const char *name, uint64_t *offset, struct ext_dir *found)
{
    struct ext_fs *f = n->fs;
    uint64_t off = 0;
    while (off < n->size) {
        struct ext_dir d;
        int r = read_locked(n, off, &d, sizeof(d));
        if (r != (int)sizeof(d))
            return r < 0 ? r : -EIO;
        if (d.length < 8 || d.length % 4 || d.length > f->block_size - off % f->block_size ||
            d.name_len > d.length - 8 || d.name_len > NAME_MAX)
            return -EIO;
        if (d.inode && strlen(name) == d.name_len) {
            char s[NAME_MAX + 1];
            r = read_locked(n, off + 8, s, d.name_len);
            if (r != d.name_len)
                return r < 0 ? r : -EIO;
            s[d.name_len] = 0;
            if (!strcmp(name, s)) {
                *offset = off;
                *found = d;
                return 0;
            }
        }
        off += d.length;
    }
    return -ENOENT;
}
static uint8_t file_type(uint32_t m)
{
    switch (m & S_IFMT) {
        case S_IFREG:
            return 1;
        case S_IFDIR:
            return 2;
        case S_IFLNK:
            return 7;
        default:
            return 0;
    }
}
static int dir_add(struct vnode *n, const char *name, uint32_t ino, uint32_t mode)
{
    struct ext_fs *f = n->fs;
    struct ext_inode *in = &((struct ext_node *)n->data)->inode;
    if (in->flags & 0x1000) {
        in->flags &= ~0x1000U;
        int converted = inode_write(f, n->ino, in);
        if (converted)
            return converted;
    }
    uint64_t oldoff;
    struct ext_dir old;
    int r = dir_find(n, name, &oldoff, &old);
    if (r != -ENOENT)
        return r ? r : -EEXIST;
    unsigned len = strlen(name), need = ALIGN_UP(8 + len, 4);
    uint64_t off = 0;
    uint8_t *buf = kzalloc(f->block_size);
    if (!buf)
        return -ENOMEM;
    while (off < n->size) {
        struct ext_dir d;
        r = read_locked(n, off, &d, 8);
        if (r != 8) {
            r = r < 0 ? r : -EIO;
            goto done;
        }
        unsigned used = d.inode ? ALIGN_UP(8 + d.name_len, 4) : 0;
        if (d.length - used >= need) {
            unsigned available = d.length - used;
            if (used) {
                d.length = used;
                r = write_locked(n, off, &d, 8);
                if (r != 8) {
                    r = r < 0 ? r : -EIO;
                    goto done;
                }
                off += used;
            }
            struct ext_dir fresh = {ino, available, len,
                                    (f->super.incompat & 2) ? file_type(mode) : 0};
            memcpy(buf, &fresh, 8);
            memcpy(buf + 8, name, len);
            r = write_locked(n, off, buf, available);
            r = r == (int)available ? 0 : (r < 0 ? r : -EIO);
            goto done;
        }
        off += d.length;
    }
    {
        struct ext_dir fresh = {ino, f->block_size, len,
                                (f->super.incompat & 2) ? file_type(mode) : 0};
        memcpy(buf, &fresh, 8);
        memcpy(buf + 8, name, len);
        r = write_locked(n, off, buf, f->block_size);
        r = r == (int)f->block_size ? 0 : (r < 0 ? r : -EIO);
    }
done:
    kfree(buf);
    return r;
}
static int dir_delete(struct vnode *n, const char *name, uint32_t *ino)
{
    struct ext_fs *f = n->fs;
    struct ext_inode *in = &((struct ext_node *)n->data)->inode;
    if (in->flags & 0x1000) {
        in->flags &= ~0x1000U;
        int converted = inode_write(f, n->ino, in);
        if (converted)
            return converted;
    }
    uint64_t off;
    struct ext_dir d;
    int r = dir_find(n, name, &off, &d);
    if (r)
        return r;
    *ino = d.inode;
    uint64_t block_start = ALIGN_DOWN(off, f->block_size), p = block_start, last = off;
    while (p < off) {
        struct ext_dir prev;
        r = read_locked(n, p, &prev, 8);
        if (r != 8 || !prev.length)
            return -EIO;
        last = p;
        p += prev.length;
    }
    if (last != off) {
        struct ext_dir prev;
        r = read_locked(n, last, &prev, 8);
        if (r != 8)
            return -EIO;
        prev.length += d.length;
        r = write_locked(n, last, &prev, 8);
    } else {
        d.inode = 0;
        r = write_locked(n, off, &d, 8);
    }
    return r == 8 ? 0 : (r < 0 ? r : -EIO);
}
static int lookup(struct vnode *n, const char *name, struct vnode **out)
{
    struct ext_fs *f = n->fs;
    spin_lock(&f->lock);
    uint64_t off;
    struct ext_dir d;
    int r = dir_find(n, name, &off, &d);
    if (!r) {
        struct vnode *v = iget(f, d.inode);
        if (!v)
            r = -EIO;
        else {
            if (S_ISDIR(v->mode) && !v->parent && v != n) {
                v->parent = n;
                ((struct ext_node *)v->data)->parent_ref = true;
                vnode_get(n);
            }
            *out = v;
        }
    }
    spin_unlock(&f->lock);
    return r;
}
static int readdir_node(struct vnode *n, uint64_t index, struct kdirent *out)
{
    struct ext_fs *f = n->fs;
    spin_lock(&f->lock);
    uint64_t off = 0;
    int result = 0;
    while (off < n->size) {
        struct ext_dir d;
        int r = read_locked(n, off, &d, 8);
        if (r != 8 || d.length < 8 || d.length % 4 ||
            d.length > f->block_size - off % f->block_size || d.name_len > d.length - 8 ||
            d.name_len > NAME_MAX) {
            result = r < 0 ? r : -EIO;
            break;
        }
        if (d.inode && index-- == 0) {
            struct ext_inode in;
            r = inode_read(f, d.inode, &in);
            if (r) {
                result = r;
                break;
            }
            memset(out, 0, sizeof(*out));
            out->ino = d.inode;
            out->mode = in.mode;
            r = read_locked(n, off + 8, out->name, d.name_len);
            result = r == d.name_len ? 1 : (r < 0 ? r : -EIO);
            break;
        }
        off += d.length;
    }
    spin_unlock(&f->lock);
    return result;
}
static int free_tree(struct ext_fs *f, uint32_t block, unsigned depth, struct ext_inode *in)
{
    if (!block)
        return 0;
    if (depth) {
        uint32_t *buf = kmalloc(f->block_size);
        if (!buf)
            return -ENOMEM;
        int r = bread(f, block, buf);
        if (!r)
            for (unsigned i = 0; i < f->block_size / 4; i++)
                if (buf[i]) {
                    r = free_tree(f, buf[i], depth - 1, in);
                    if (r)
                        break;
                }
        kfree(buf);
        if (r)
            return r;
    }
    int r = free_bit(f, false, block);
    if (!r) {
        if (in->sectors < f->block_size / 512)
            return -EIO;
        in->sectors -= f->block_size / 512;
    }
    return r;
}
static int clear_blocks(struct vnode *n)
{
    struct ext_fs *f = n->fs;
    struct ext_inode *in = &((struct ext_node *)n->data)->inode;
    if ((n->mode & S_IFMT) == S_IFLNK && n->size <= 60 && !in->sectors)
        memset(in->block, 0, 60);
    else
        for (unsigned i = 0; i < 15; i++)
            if (in->block[i]) {
                int r = free_tree(f, in->block[i], i < 12 ? 0 : i - 11, in);
                if (r)
                    return r;
                in->block[i] = 0;
            }
    in->size = in->size_high = 0;
    n->size = 0;
    return inode_write(f, n->ino, in);
}
static int prune_tree(struct ext_fs *f, uint32_t *root, unsigned depth, uint64_t first,
                      uint64_t keep, struct ext_inode *in)
{
    if (!*root)
        return 0;
    uint64_t capacity = 1, per = f->block_size / 4;
    for (unsigned i = 0; i < depth; i++)
        capacity *= per;
    if (keep >= first + capacity)
        return 0;
    if (keep <= first) {
        int r = free_tree(f, *root, depth, in);
        if (!r)
            *root = 0;
        return r;
    }
    if (!depth)
        return 0;
    uint32_t *table = kmalloc(f->block_size);
    if (!table)
        return -ENOMEM;
    int r = bread(f, *root, table);
    bool empty = true;
    if (!r)
        for (unsigned i = 0; i < per; i++) {
            r = prune_tree(f, &table[i], depth - 1, first + i * (capacity / per), keep, in);
            if (r)
                break;
            if (table[i])
                empty = false;
        }
    if (!r) {
        if (empty) {
            r = free_bit(f, false, *root);
            if (!r) {
                in->sectors -= f->block_size / 512;
                *root = 0;
            }
        } else
            r = bwrite(f, *root, table);
    }
    kfree(table);
    return r;
}
static int truncate_locked(struct vnode *n, uint64_t size)
{
    struct ext_fs *f = n->fs;
    struct ext_inode *in = &((struct ext_node *)n->data)->inode;
    if (f->failed)
        return -EROFS;
    if (in->flags & 0x30)
        return -EPERM;
    uint64_t per = f->block_size / 4,
             max = (12 + per + per * per + per * per * per) * f->block_size;
    if (size > max)
        return -EFBIG;
    if (size < n->size) {
        uint64_t keep = (size + f->block_size - 1) / f->block_size;
        for (unsigned i = 0; i < 12; i++) {
            int r = prune_tree(f, &in->block[i], 0, i, keep, in);
            if (r)
                return r;
        }
        uint64_t first = 12, span = per;
        for (unsigned depth = 1; depth <= 3; depth++) {
            int r = prune_tree(f, &in->block[11 + depth], depth, first, keep, in);
            if (r)
                return r;
            first += span;
            span *= per;
        }
        if (size % f->block_size) {
            uint32_t b;
            int r = data_block(n, size / f->block_size, false, &b);
            if (r)
                return r;
            if (b) {
                uint8_t *buf = kmalloc(f->block_size);
                if (!buf)
                    return -ENOMEM;
                r = bread(f, b, buf);
                if (!r) {
                    memset(buf + size % f->block_size, 0, f->block_size - size % f->block_size);
                    r = bwrite(f, b, buf);
                }
                kfree(buf);
                if (r)
                    return r;
            }
        }
    }
    n->size = size;
    in->size = size;
    in->size_high = size >> 32;
    in->mtime = in->ctime = now_seconds();
    refresh(n);
    return inode_write(f, n->ino, in);
}
static int truncate_node(struct vnode *n, uint64_t size)
{
    if ((n->mode & S_IFMT) != S_IFREG)
        return -EINVAL;
    struct ext_fs *f = n->fs;
    spin_lock(&f->lock);
    int r = truncate_locked(n, size);
    if (!r)
        r = sync_meta(f);
    mutation_failed(f, r);
    spin_unlock(&f->lock);
    return r;
}
static int create(struct vnode *p, const char *name, uint32_t mode, const char *target,
                  struct vnode **out)
{
    struct ext_fs *f = p->fs;
    spin_lock(&f->lock);
    int r = 0;
    uint32_t ino = 0;
    struct vnode *n = NULL;
    uint64_t off;
    struct ext_dir d;
    if (f->failed) {
        r = -EROFS;
        goto done;
    }
    r = dir_find(p, name, &off, &d);
    if (!r) {
        r = -EEXIST;
        goto done;
    }
    if (r != -ENOENT)
        goto done;
    r = alloc_bit(f, true, (p->ino - 1) / f->super.inodes_group, &ino);
    if (r)
        goto done;
    struct ext_inode in = {0};
    in.mode = mode;
    in.links = S_ISDIR(mode) ? 2 : 1;
    in.atime = in.mtime = in.ctime = now_seconds();
    void *zero = kzalloc(f->super.inode_size);
    if (!zero) {
        r = -ENOMEM;
        goto rollback;
    }
    r = disk_bytes(f, inode_offset(f, ino), zero, f->super.inode_size, true);
    kfree(zero);
    if (r)
        goto rollback;
    r = inode_write(f, ino, &in);
    if (r)
        goto rollback;
    n = iget(f, ino);
    if (!n) {
        r = -ENOMEM;
        goto rollback;
    }
    if (S_ISDIR(mode)) {
        r = dir_add(n, ".", ino, mode);
        if (!r)
            r = dir_add(n, "..", p->ino, p->mode);
        if (r)
            goto rollback;
    }
    if ((mode & S_IFMT) == S_IFLNK) {
        size_t len = strlen(target);
        if (len <= 60) {
            memcpy(((struct ext_node *)n->data)->inode.block, target, len);
            ((struct ext_node *)n->data)->inode.size = len;
            n->size = len;
            r = inode_write(f, ino, &((struct ext_node *)n->data)->inode);
        } else {
            r = write_locked(n, 0, target, len);
            r = r == (int)len ? 0 : (r < 0 ? r : -EIO);
        }
        if (r)
            goto rollback;
    }
    r = dir_add(p, name, ino, mode);
    if (r)
        goto rollback;
    if (S_ISDIR(mode)) {
        struct ext_inode *pi = &((struct ext_node *)p->data)->inode;
        if (pi->links == UINT16_MAX) {
            uint32_t ignored;
            dir_delete(p, name, &ignored);
            r = -EMLINK;
            goto rollback;
        }
        pi->links++;
        refresh(p);
        f->groups[(ino - 1) / f->super.inodes_group].used_dirs++;
        r = inode_write(f, p->ino, pi);
        n->parent = p;
        ((struct ext_node *)n->data)->parent_ref = true;
        vnode_get(p);
    }
    if (!r)
        r = sync_meta(f);
    if (!r) {
        *out = n;
        n = NULL;
    }
    goto done;
rollback:
    if (n) {
        clear_blocks(n);
        ((struct ext_node *)n->data)->inode.links = 0;
        ((struct ext_node *)n->data)->inode.mode = 0;
        inode_write(f, ino, &((struct ext_node *)n->data)->inode);
        n->nlink = 1;
    }
    if (ino)
        free_bit(f, true, ino);
done:
    mutation_failed(f, r);
    spin_unlock(&f->lock);
    vnode_put(n);
    return r;
}
static bool directory_empty(struct vnode *n, int *err)
{
    uint64_t off = 0;
    while (off < n->size) {
        struct ext_dir d;
        int r = read_locked(n, off, &d, 8);
        if (r != 8 || d.length < 8 || d.name_len > NAME_MAX || d.name_len > d.length - 8) {
            *err = -EIO;
            return false;
        }
        if (d.inode) {
            char name[NAME_MAX + 1];
            r = read_locked(n, off + 8, name, d.name_len);
            if (r != d.name_len) {
                *err = -EIO;
                return false;
            }
            name[d.name_len] = 0;
            if (strcmp(name, ".") && strcmp(name, ".."))
                return false;
        }
        off += d.length;
    }
    return true;
}
static int remove_node(struct vnode *p, const char *name, bool dir)
{
    struct ext_fs *f = p->fs;
    struct vnode *n = NULL;
    spin_lock(&f->lock);
    int r;
    uint64_t off;
    struct ext_dir d;
    if (f->failed) {
        r = -EROFS;
        goto done;
    }
    r = dir_find(p, name, &off, &d);
    if (r)
        goto done;
    n = iget(f, d.inode);
    if (!n) {
        r = -EIO;
        goto done;
    }
    if (dir != S_ISDIR(n->mode)) {
        r = dir ? -ENOTDIR : -EISDIR;
        goto done;
    }
    if (n->mounted || (dir && atomic_load(&n->refs) > 1)) {
        r = -EBUSY;
        goto done;
    }
    struct ext_inode *in = &((struct ext_node *)n->data)->inode;
    if (in->flags & 0x30) {
        r = -EPERM;
        goto done;
    }
    if (dir) {
        int err = 0;
        if (!directory_empty(n, &err)) {
            r = err ? err : -ENOTEMPTY;
            goto done;
        }
    }
    uint32_t ignored;
    r = dir_delete(p, name, &ignored);
    if (r)
        goto done;
    if (dir) {
        in->links = 0;
        struct ext_inode *pi = &((struct ext_node *)p->data)->inode;
        if (pi->links)
            pi->links--;
        refresh(p);
        f->groups[(n->ino - 1) / f->super.inodes_group].used_dirs--;
        r = inode_write(f, p->ino, pi);
    } else if (in->links)
        in->links--;
    refresh(n);
    if (!r)
        r = inode_write(f, n->ino, in);
    if (!r)
        r = sync_meta(f);
done:
    mutation_failed(f, r);
    spin_unlock(&f->lock);
    vnode_put(n);
    return r;
}
static int link_node(struct vnode *p, const char *name, struct vnode *n)
{
    if (n->fs != p->fs)
        return -EXDEV;
    if (S_ISDIR(n->mode))
        return -EPERM;
    struct ext_fs *f = p->fs;
    spin_lock(&f->lock);
    struct ext_inode *in = &((struct ext_node *)n->data)->inode;
    int r;
    if (f->failed)
        r = -EROFS;
    else if (in->flags & 0x30)
        r = -EPERM;
    else if (in->links == UINT16_MAX)
        r = -EMLINK;
    else {
        r = dir_add(p, name, n->ino, n->mode);
        if (!r) {
            in->links++;
            in->ctime = now_seconds();
            refresh(n);
            r = inode_write(f, n->ino, in);
            if (!r)
                r = sync_meta(f);
        }
    }
    mutation_failed(f, r);
    spin_unlock(&f->lock);
    return r;
}
static int chmod_node(struct vnode *n, uint32_t mode)
{
    struct ext_fs *f = n->fs;
    spin_lock(&f->lock);
    struct ext_inode *in = &((struct ext_node *)n->data)->inode;
    int r = 0;
    if (in->flags & 0x10)
        r = -EPERM;
    else {
        in->mode = (in->mode & S_IFMT) | (mode & 07777);
        in->ctime = now_seconds();
        refresh(n);
        r = inode_write(f, n->ino, in);
        if (!r)
            r = sync_meta(f);
    }
    mutation_failed(f, r);
    spin_unlock(&f->lock);
    return r;
}
static int chown_node(struct vnode *n, uint32_t uid, uint32_t gid)
{
    struct ext_fs *f = n->fs;
    spin_lock(&f->lock);
    struct ext_inode *in = &((struct ext_node *)n->data)->inode;
    int r = 0;
    if (in->flags & 0x10)
        r = -EPERM;
    else {
        if (uid != UINT32_MAX) {
            in->uid = uid;
            in->osd2[4] = uid >> 16;
            in->osd2[5] = uid >> 24;
        }
        if (gid != UINT32_MAX) {
            in->gid = gid;
            in->osd2[6] = gid >> 16;
            in->osd2[7] = gid >> 24;
        }
        in->mode &= ~06000U;
        in->ctime = now_seconds();
        refresh(n);
        r = inode_write(f, n->ino, in);
        if (!r)
            r = sync_meta(f);
    }
    mutation_failed(f, r);
    spin_unlock(&f->lock);
    return r;
}
static void destroy(struct vnode *n)
{
    struct ext_fs *f = n->fs;
    struct ext_node *e = n->data;
    struct vnode *parent = e->parent_ref ? n->parent : NULL;
    spin_lock(&f->lock);
    if (!n->nlink && !f->failed) {
        int r = clear_blocks(n);
        if (!r) {
            e->inode.mode = 0;
            e->inode.dtime = now_seconds();
            r = inode_write(f, n->ino, &e->inode);
        }
        if (!r)
            r = free_bit(f, true, n->ino);
        if (!r)
            r = sync_meta(f);
        if (r) {
            f->failed = true;
            kprintf("ext2 inode reclamation error %d\n", r);
        }
    }
    struct ext_node **p = &f->nodes;
    while (*p && *p != e)
        p = &(*p)->next;
    if (*p)
        *p = e->next;
    bool last = !--f->node_count;
    spin_unlock(&f->lock);
    kfree(e);
    vnode_put(parent);
    if (last) {
        spin_lock(&filesystems_lock);
        struct ext_fs **link = &mounted_filesystems;
        while (*link && *link != f)
            link = &(*link)->next;
        if (*link)
            *link = f->next;
        spin_unlock(&filesystems_lock);
        kfree(f->groups);
        kfree(f);
    }
}
static struct vnode_ops ops = {read_node,   write_node, lookup,       create,
                               remove_node, link_node,  readdir_node, truncate_node,
                               chmod_node,  chown_node, destroy};
int ext2_mount(struct blockdev *b, struct vnode **root)
{
    spin_lock(&filesystems_lock);
    for (struct ext_fs *old = mounted_filesystems; old; old = old->next)
        if (old->disk == b) {
            spin_unlock(&filesystems_lock);
            return -EBUSY;
        }
    spin_unlock(&filesystems_lock);
    struct ext_fs *f = kzalloc(sizeof(*f));
    if (!f)
        return -ENOMEM;
    f->disk = b;
    atomic_flag_clear(&f->lock.flag);
    int r = disk_bytes(f, 1024, &f->super, 1024, false);
    if (r)
        goto fail;
    struct ext_super *s = &f->super;
    if (s->magic != 0xef53) {
        r = -EINVAL;
        goto fail;
    }
    if (s->revision > 1 || s->log_block > 2 || s->log_frag != s->log_block || !s->blocks_group ||
        !s->inodes_group || !s->inodes || !s->blocks || s->incompat & ~2U || s->rocompat & ~3U ||
        s->compat & 4 || s->creator != 0 || (s->state != 1 && s->state != 2)) {
        r = -ENOTSUP;
        goto fail;
    }
    f->block_size = 1024U << s->log_block;
    f->gdt_start = s->first_data + 1;
    if (!s->revision) {
        s->inode_size = 128;
        s->first_inode = 11;
    }
    if (s->inode_size < 128 || s->inode_size > f->block_size ||
        (s->inode_size & (s->inode_size - 1)) || s->blocks_group > f->block_size * 8 ||
        s->inodes_group > f->block_size * 8 || s->blocks > (b->sectors / (f->block_size / 512)) ||
        s->first_data != (f->block_size == 1024 ? 1U : 0U) || s->free_blocks > s->blocks ||
        s->free_inodes > s->inodes) {
        r = -EINVAL;
        goto fail;
    }
    f->group_count = (s->blocks - s->first_data + s->blocks_group - 1) / s->blocks_group;
    if (!f->group_count || f->group_count > 65536 ||
        (uint64_t)f->group_count * s->inodes_group < s->inodes) {
        r = -EINVAL;
        goto fail;
    }
    f->groups = kmalloc(f->group_count * sizeof(struct ext_group));
    if (!f->groups) {
        r = -ENOMEM;
        goto fail;
    }
    r = disk_bytes(f, (uint64_t)f->gdt_start * f->block_size, f->groups,
                   f->group_count * sizeof(struct ext_group), false);
    if (r)
        goto fail;
    for (unsigned g = 0; g < f->group_count; g++) {
        struct ext_group *d = &f->groups[g];
        uint64_t table_blocks =
            ALIGN_UP((uint64_t)s->inodes_group * s->inode_size, f->block_size) / f->block_size;
        if (d->block_bitmap >= s->blocks || d->inode_bitmap >= s->blocks ||
            d->inode_table >= s->blocks || table_blocks > s->blocks - d->inode_table ||
            d->free_blocks > s->blocks_group || d->free_inodes > s->inodes_group) {
            r = -EIO;
            goto fail;
        }
    }
    struct vnode *n = iget(f, 2);
    if (!n) {
        r = -EIO;
        goto fail;
    }
    if (!S_ISDIR(n->mode)) {
        vnode_put(n);
        return -EIO;
    }
    n->parent = n;
    f->failed = b->readonly || s->state != 1;
    if (s->state != 1)
        kprintf("ext2: %s unclean, mounted read-only; offline fsck required\n", b->name);
    spin_lock(&filesystems_lock);
    for (struct ext_fs *old = mounted_filesystems; old; old = old->next)
        if (old->disk == b) {
            spin_unlock(&filesystems_lock);
            vnode_put(n);
            return -EBUSY;
        }
    f->next = mounted_filesystems;
    mounted_filesystems = f;
    spin_unlock(&filesystems_lock);
    *root = n;
    kprintf("ext2: %s block=%u groups=%u %s mounted\n", b->name, f->block_size, f->group_count,
            f->failed ? "read-only" : "read/write");
    return 0;
fail:
    kfree(f->groups);
    kfree(f);
    return r;
}
