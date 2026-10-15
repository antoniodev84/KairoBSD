#include <kernel.h>
struct ahci_port {
    volatile uint32_t clb, clbu, fb, fbu, is, ie, cmd, reserved, tfd, sig, ssts, sctl, serr, sact,
        ci, sntf, fbs, devslp, reserved2[10], vendor[4];
};
struct ahci_hba {
    volatile uint32_t cap, ghc, is, pi, vs, ccc_ctl, ccc_ports, em_loc, em_ctl, cap2, bohc;
    uint8_t reserved[0xa0 - 0x2c], vendor[0x60];
    struct ahci_port ports[32];
};
struct ahci_header {
    uint16_t flags, prdtl;
    uint32_t transferred, ctba, ctbau, reserved[4];
};
struct ahci_prdt {
    uint32_t dba, dbau, reserved, count;
};
struct ahci_table {
    uint8_t cfis[64], acmd[16], reserved[48];
    struct ahci_prdt prdt;
};
struct sata {
    struct ahci_port *port;
    uint64_t list, fis, table, bounce;
    bool failed;
    spinlock_t lock;
};
static unsigned disks;
static bool wait_clear(volatile uint32_t *p, uint32_t bits, uint64_t timeout)
{
    uint64_t start = monotonic_ns();
    while (*p & bits) {
        if (monotonic_ns() - start > timeout)
            return false;
        pause_cpu();
    }
    return true;
}
static int issue(struct sata *s, uint8_t cmd, uint64_t lba, void *buffer, bool write, bool data)
{
    if (s->failed)
        return -EIO;
    struct ahci_port *p = s->port;
    if (!wait_clear(&p->tfd, 0x88, 2000000000UL)) {
        s->failed = true;
        return -ETIMEDOUT;
    }
    struct ahci_header *h = phys_ptr(s->list);
    struct ahci_table *t = phys_ptr(s->table);
    memset(h, 0, sizeof(*h));
    memset(t, 0, sizeof(*t));
    h->flags = 5 | (write ? 64 : 0);
    h->prdtl = data ? 1 : 0;
    h->ctba = s->table;
    h->ctbau = s->table >> 32;
    t->cfis[0] = 0x27;
    t->cfis[1] = 0x80;
    t->cfis[2] = cmd;
    t->cfis[4] = lba;
    t->cfis[5] = lba >> 8;
    t->cfis[6] = lba >> 16;
    t->cfis[7] = 0x40;
    t->cfis[8] = lba >> 24;
    t->cfis[9] = lba >> 32;
    t->cfis[10] = lba >> 40;
    if (data)
        t->cfis[12] = 1;
    if (data) {
        t->prdt = (struct ahci_prdt){s->bounce, s->bounce >> 32, 0, 511};
        if (write)
            memcpy(phys_ptr(s->bounce), buffer, 512);
    }
    p->is = UINT32_MAX;
    p->serr = UINT32_MAX;
    atomic_thread_fence(memory_order_seq_cst);
    p->ci = 1;
    if (!wait_clear(&p->ci, 1, 2000000000UL)) {
        s->failed = true;
        return -ETIMEDOUT;
    }
    atomic_thread_fence(memory_order_acquire);
    if ((p->is & (1U << 30)) || (p->tfd & 1))
        return -EIO;
    if (data && !write)
        memcpy(buffer, phys_ptr(s->bounce), 512);
    return 0;
}
static int transfer(struct blockdev *b, uint64_t lba, uint32_t n, void *buf, bool write)
{
    struct sata *s = b->data;
    if (lba > 0xffffffffffffUL || n > 0x1000000000000UL - lba)
        return -EINVAL;
    uint64_t f = irq_save();
    spin_lock(&s->lock);
    int r = 0;
    for (uint32_t i = 0; i < n; i++) {
        r = issue(s, write ? 0x35 : 0x25, lba + i, (uint8_t *)buf + i * 512, write, true);
        if (r)
            break;
    }
    spin_unlock(&s->lock);
    irq_restore(f);
    return r;
}
static int flush(struct blockdev *b)
{
    struct sata *s = b->data;
    uint64_t f = irq_save();
    spin_lock(&s->lock);
    int r = issue(s, 0xea, 0, NULL, false, false);
    spin_unlock(&s->lock);
    irq_restore(f);
    return r;
}
int ahci_probe(struct pci_device *d)
{
    if (!d->bar[5] || d->bar_size[5] < 0x180)
        return -ENODEV;
    pci_write(d, 4, 2, pci_read(d, 4, 2) | 6 | (1U << 10));
    struct ahci_hba *h = mmio_map(d->bar[5], d->bar_size[5]);
    if (h->cap2 & 1) {
        h->bohc |= 2;
        if (!wait_clear(&h->bohc, 0x11, 2000000000UL))
            return -ETIMEDOUT;
    }
    h->ghc = (h->ghc | (1U << 31)) & ~2U;
    uint32_t implemented = h->pi;
    int result = 0;
    for (unsigned i = 0; i < 32 && 0x100 + (i + 1) * 0x80 <= d->bar_size[5]; i++)
        if (implemented & (1U << i)) {
            struct ahci_port *p = &h->ports[i];
            if ((p->ssts & 15) != 3 || ((p->ssts >> 8) & 15) != 1 || p->sig != 0x101)
                continue;
            p->cmd &= ~1U;
            if (!wait_clear(&p->cmd, 1U << 15, 1000000000UL)) {
                result = -ETIMEDOUT;
                continue;
            }
            p->cmd &= ~16U;
            if (!wait_clear(&p->cmd, 1U << 14, 1000000000UL)) {
                result = -ETIMEDOUT;
                continue;
            }
            struct sata *s = kzalloc(sizeof(*s));
            if (!s)
                return -ENOMEM;
            s->port = p;
            atomic_flag_clear(&s->lock.flag);
            s->list = frame_alloc();
            s->fis = frame_alloc();
            s->table = frame_alloc();
            s->bounce = frame_alloc();
            if (!s->list || !s->fis || !s->table || !s->bounce ||
                (!(h->cap & (1U << 31)) && (s->list | s->fis | s->table | s->bounce) >> 32)) {
                if (s->list)
                    frame_put(s->list);
                if (s->fis)
                    frame_put(s->fis);
                if (s->table)
                    frame_put(s->table);
                if (s->bounce)
                    frame_put(s->bounce);
                kfree(s);
                return -ENOMEM;
            }
            p->clb = s->list;
            p->clbu = s->list >> 32;
            p->fb = s->fis;
            p->fbu = s->fis >> 32;
            p->ie = 0;
            p->is = UINT32_MAX;
            p->serr = UINT32_MAX;
            p->cmd |= 16;
            p->cmd |= 1;
            uint16_t identify[256];
            int r = issue(s, 0xec, 0, identify, false, true);
            if (r || !(identify[83] & (1U << 10)) ||
                ((identify[106] & 0xd000) == 0x5000 && (identify[117] != 256 || identify[118]))) {
                p->cmd &= ~1U;
                if (wait_clear(&p->cmd, 1U << 15, 1000000000UL)) {
                    p->cmd &= ~16U;
                    if (wait_clear(&p->cmd, 1U << 14, 1000000000UL)) {
                        frame_put(s->list);
                        frame_put(s->fis);
                        frame_put(s->table);
                        frame_put(s->bounce);
                        kfree(s);
                    }
                }
                result = r ? r : -ENOTSUP;
                continue;
            }
            uint64_t sectors = (uint64_t)identify[100] | ((uint64_t)identify[101] << 16) |
                               ((uint64_t)identify[102] << 32) | ((uint64_t)identify[103] << 48);
            struct blockdev b = {.sectors = sectors,
                                 .sector_size = 512,
                                 .data = s,
                                 .transfer = transfer,
                                 .flush = flush};
            snprintf(b.name, sizeof(b.name), "sd%u", disks++);
            r = block_register(&b);
            if (r)
                result = r;
            else
                kprintf("AHCI: SATA port %u DMA operational\n", i);
        }
    return result;
}
