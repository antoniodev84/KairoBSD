#include <kernel.h>
struct vcommon {
    uint32_t feature_select, feature, driver_select, driver_feature;
    uint16_t msix, numq;
    uint8_t status, generation;
    uint16_t qselect, qsize, qmsix, qenable, qnotify;
    uint64_t desc, avail, used;
} PACKED;
struct vdesc {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
};
struct vavail {
    uint16_t flags, idx, ring[8], event;
};
struct vused {
    uint16_t flags, idx;
    struct {
        uint32_t id, len;
    } ring[8];
    uint16_t event;
};
struct vblk {
    volatile struct vcommon *common;
    volatile uint16_t *notify;
    volatile uint64_t *capacity;
    struct vdesc *desc;
    volatile struct vavail *avail;
    volatile struct vused *used;
    uint64_t queue, request;
    uint16_t last;
    bool readonly, can_flush, failed;
    spinlock_t lock;
};
static unsigned disks;
static void *cap_map(struct pci_device *d, unsigned cap, unsigned need)
{
    unsigned bar = pci_read(d, cap + 4, 1);
    uint32_t off = pci_read(d, cap + 8, 4), len = pci_read(d, cap + 12, 4);
    if (bar >= 6 || !d->bar[bar] || !d->bar_size[bar] || off > d->bar_size[bar] ||
        len > d->bar_size[bar] - off || len < need || (pci_read(d, 16 + bar * 4, 4) & 1))
        return NULL;
    return mmio_map(d->bar[bar] + off, len);
}
static int request(struct vblk *v, uint32_t type, uint64_t sector, void *buf, bool write)
{
    if (v->failed)
        return -EIO;
    uint8_t *p = phys_ptr(v->request);
    struct {
        uint32_t type, reserved;
        uint64_t sector;
    } h = {type, 0, sector};
    memcpy(p, &h, 16);
    p[32] = 255;
    bool flush = type == 4;
    if (!flush && write)
        memcpy(p + 512, buf, 512);
    v->desc[0] = (struct vdesc){v->request, 16, 1, 1};
    if (flush)
        v->desc[1] = (struct vdesc){v->request + 32, 1, 2, 0};
    else {
        v->desc[1] = (struct vdesc){v->request + 512, 512, (uint16_t)(1 | (write ? 0 : 2)), 2};
        v->desc[2] = (struct vdesc){v->request + 32, 1, 2, 0};
    }
    uint16_t a = v->avail->idx;
    v->avail->ring[a % 8] = 0;
    atomic_thread_fence(memory_order_release);
    v->avail->idx = a + 1;
    atomic_thread_fence(memory_order_seq_cst);
    *v->notify = 0;
    uint64_t start = monotonic_ns();
    while (v->used->idx == v->last) {
        if (monotonic_ns() - start > 2000000000UL) {
            v->failed = true;
            v->common->status |= 128;
            return -ETIMEDOUT;
        }
        pause_cpu();
    }
    atomic_thread_fence(memory_order_acquire);
    if (v->used->ring[v->last % 8].id != 0) {
        v->failed = true;
        return -EIO;
    }
    v->last++;
    if (p[32])
        return p[32] == 2 ? -ENOTSUP : -EIO;
    if (!flush && !write)
        memcpy(buf, p + 512, 512);
    return 0;
}
static int transfer(struct blockdev *b, uint64_t lba, uint32_t n, void *buf, bool write)
{
    struct vblk *v = b->data;
    if (write && v->readonly)
        return -EROFS;
    uint64_t f = irq_save();
    spin_lock(&v->lock);
    int r = 0;
    for (uint32_t i = 0; i < n; i++) {
        r = request(v, write ? 1 : 0, lba + i, (uint8_t *)buf + i * 512, write);
        if (r)
            break;
    }
    spin_unlock(&v->lock);
    irq_restore(f);
    return r;
}
static int flush(struct blockdev *b)
{
    struct vblk *v = b->data;
    if (!v->can_flush)
        return -ENOTSUP;
    uint64_t f = irq_save();
    spin_lock(&v->lock);
    int r = request(v, 4, 0, NULL, false);
    spin_unlock(&v->lock);
    irq_restore(f);
    return r;
}
int virtio_probe(struct pci_device *d)
{
    if (!(pci_read(d, 6, 2) & 16))
        return -ENOTSUP;
    struct vblk *v = kzalloc(sizeof(*v));
    if (!v)
        return -ENOMEM;
    atomic_flag_clear(&v->lock.flag);
    void *notify_base = NULL;
    uint32_t multiplier = 0, notify_len = 0;
    unsigned cap = pci_read(d, 0x34, 1) & 252;
    uint64_t seen[4] = {0};
    while (cap) {
        if (cap < 0x40 || cap > 0xfc || (seen[cap / 64] & (1UL << (cap % 64)))) {
            kfree(v);
            return -EINVAL;
        }
        seen[cap / 64] |= 1UL << (cap % 64);
        unsigned next = pci_read(d, cap + 1, 1) & 252;
        if (pci_read(d, cap, 1) == 9 && pci_read(d, cap + 2, 1) >= 16) {
            unsigned type = pci_read(d, cap + 3, 1);
            if (type == 1)
                v->common = cap_map(d, cap, sizeof(struct vcommon));
            if (type == 2 && pci_read(d, cap + 2, 1) >= 20) {
                notify_base = cap_map(d, cap, 2);
                multiplier = pci_read(d, cap + 16, 4);
                notify_len = pci_read(d, cap + 12, 4);
            }
            if (type == 4)
                v->capacity = cap_map(d, cap, 8);
        }
        cap = next;
    }
    if (!v->common || !v->capacity || !notify_base) {
        kfree(v);
        return -ENOTSUP;
    }
    pci_write(d, 4, 2, pci_read(d, 4, 2) | 6 | (1U << 10));
    v->common->status = 0;
    atomic_thread_fence(memory_order_seq_cst);
    v->common->status = 1;
    v->common->status = 3;
    v->common->feature_select = 1;
    if (!(v->common->feature & 1)) {
        v->common->status = 128;
        kfree(v);
        return -ENOTSUP;
    }
    v->common->feature_select = 0;
    uint32_t features = v->common->feature;
    v->readonly = features & (1U << 5);
    v->can_flush = features & (1U << 9);
    v->common->driver_select = 0;
    v->common->driver_feature = features & ((1U << 5) | (1U << 9));
    v->common->driver_select = 1;
    v->common->driver_feature = 1;
    v->common->status |= 8;
    atomic_thread_fence(memory_order_seq_cst);
    if (!(v->common->status & 8)) {
        v->common->status = 128;
        kfree(v);
        return -ENOTSUP;
    }
    v->common->qselect = 0;
    if (v->common->qsize < 8 || v->common->qenable) {
        v->common->status = 128;
        kfree(v);
        return -EINVAL;
    }
    uint64_t notify_off = (uint64_t)v->common->qnotify * multiplier;
    if (notify_off + 2 > notify_len) {
        v->common->status = 128;
        kfree(v);
        return -EINVAL;
    }
    v->notify = (void *)((uint8_t *)notify_base + notify_off);
    v->queue = frame_alloc();
    v->request = frame_alloc();
    if (!v->queue || !v->request) {
        v->common->status = 128;
        if (v->queue)
            frame_put(v->queue);
        if (v->request)
            frame_put(v->request);
        kfree(v);
        return -ENOMEM;
    }
    v->desc = phys_ptr(v->queue);
    v->avail = phys_ptr(v->queue + 256);
    v->used = phys_ptr(v->queue + 512);
    v->avail->flags = 1;
    v->common->qsize = 8;
    v->common->qmsix = 0xffff;
    v->common->desc = v->queue;
    v->common->avail = v->queue + 256;
    v->common->used = v->queue + 512;
    v->common->qenable = 1;
    v->common->status |= 4;
    uint64_t capacity;
    uint8_t g;
    do {
        g = v->common->generation;
        capacity = *v->capacity;
        atomic_thread_fence(memory_order_acquire);
    } while (g != v->common->generation);
    struct blockdev b = {.sectors = capacity,
                         .sector_size = 512,
                         .readonly = v->readonly,
                         .data = v,
                         .transfer = transfer,
                         .flush = flush};
    snprintf(b.name, sizeof(b.name), "vd%u", disks++);
    int r = block_register(&b);
    if (r)
        v->common->status = 128;
    return r;
}
