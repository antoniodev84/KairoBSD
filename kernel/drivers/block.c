#include <kernel.h>
struct blockdev blocks[32];
atomic_uint block_count;
int block_register(struct blockdev *b)
{
    if (block_count == 32 || !b->sectors || b->sectors > UINT64_MAX / 512 ||
        b->sector_size != 512 || !b->transfer)
        return -EINVAL;
    for (unsigned i = 0; i < block_count; i++)
        if (!strcmp(blocks[i].name, b->name))
            return -EEXIST;
    unsigned index = block_count;
    blocks[index] = *b;
    int r = devfs_register_block(index);
    if (r)
        return r;
    atomic_store_explicit(&block_count, index + 1, memory_order_release);
    kprintf("Block: %s, %lu sectors\n", b->name, b->sectors);
    return 0;
}
int block_io(struct blockdev *b, uint64_t lba, uint32_t n, void *p, bool write)
{
    if (!b || !p || lba > b->sectors || n > b->sectors - lba)
        return -EINVAL;
    if (!n)
        return 0;
    if (write && b->readonly)
        return -EROFS;
    if (b->parent)
        return block_io(b->parent, b->start + lba, n, p, write);
    return b->transfer(b, lba, n, p, write);
}
static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = ~0U;
    while (len--) {
        c ^= *p++;
        for (unsigned i = 0; i < 8; i++)
            c = (c >> 1) ^ (0xedb88320U & -(c & 1));
    }
    return ~c;
}
struct mbr_entry {
    uint8_t status, first[3], type, last[3];
    uint32_t start, count;
} PACKED;
struct gpt_header {
    uint8_t sig[8];
    uint32_t revision, size, crc, reserved;
    uint64_t current, backup, first, last;
    uint8_t guid[16];
    uint64_t entries;
    uint32_t count, entry_size, entries_crc;
} PACKED;
struct gpt_entry {
    uint8_t type[16], guid[16];
    uint64_t first, last, attributes;
    uint16_t name[36];
} PACKED;
struct partition {
    uint64_t start, count;
    unsigned index;
    bool readonly;
};
static int stage_partition(struct blockdev *disk, struct partition *parts, unsigned *count,
                           unsigned index, uint64_t first, uint64_t length, bool readonly)
{
    if (!first || !length || first >= disk->sectors || length > disk->sectors - first)
        return -EINVAL;
    if (*count >= 32 || block_count + *count >= 32)
        return -ENOSPC;
    for (unsigned i = 0; i < *count; i++)
        if (first < parts[i].start + parts[i].count && parts[i].start < first + length)
            return -EINVAL;
    parts[*count] = (struct partition){first, length, index, readonly};
    (*count)++;
    return 0;
}
static void publish_partitions(struct blockdev *disk, struct partition *parts, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        struct blockdev p = *disk;
        snprintf(p.name, sizeof(p.name), "%sp%u", disk->name, parts[i].index);
        p.parent = disk;
        p.start = parts[i].start;
        p.sectors = parts[i].count;
        p.readonly = disk->readonly || parts[i].readonly;
        int r = block_register(&p);
        if (r)
            panic("Partition registration invariant %d", r);
    }
}
static int gpt_read(struct blockdev *b, uint64_t at, struct gpt_header *h, uint8_t **entries)
{
    uint8_t buf[512];
    int r = block_io(b, at, 1, buf, false);
    if (r)
        return r;
    memcpy(h, buf, sizeof(*h));
    if (memcmp(h->sig, "EFI PART", 8) || h->revision != 0x10000 || h->size < 92 || h->size > 512 ||
        h->reserved || h->current != at || h->backup >= b->sectors || h->backup == at ||
        h->first > h->last || h->last >= b->sectors || h->first < 2 ||
        h->backup != (at == 1 ? b->sectors - 1 : 1) || h->entry_size < 128 ||
        h->entry_size > 4096 || (h->entry_size & (h->entry_size - 1)) || !h->count ||
        h->count > 4096)
        return -EINVAL;
    memset(buf + 16, 0, 4);
    if (crc32(buf, h->size) != h->crc)
        return -EIO;
    uint64_t bytes = (uint64_t)h->count * h->entry_size, n = ALIGN_UP(bytes, 512) / 512;
    if (h->entries >= b->sectors || n > b->sectors - h->entries ||
        (h->entries <= h->last && h->entries + n > h->first))
        return -EINVAL;
    if ((at == 1 && (h->entries < 2 || h->entries + n > h->first)) ||
        (at != 1 && (h->entries <= h->last || h->entries + n > at)))
        return -EINVAL;
    uint8_t *p = kmalloc(n * 512);
    if (!p)
        return -ENOMEM;
    r = block_io(b, h->entries, n, p, false);
    if (r || crc32(p, bytes) != h->entries_crc) {
        kfree(p);
        return r ? r : -EIO;
    }
    *entries = p;
    return 0;
}
static bool extended_type(unsigned type)
{
    return type == 5 || type == 15 || type == 0x85;
}
static int scan_mbr(struct blockdev *disk, struct mbr_entry *primary, struct partition *parts,
                    unsigned *count)
{
    unsigned extended = 0, logical = 5;
    for (unsigned i = 0; i < 4; i++) {
        struct mbr_entry *p = &primary[i];
        if (!p->type) {
            if (p->count || p->start)
                return -EINVAL;
            continue;
        }
        if (!p->start || !p->count || p->start >= disk->sectors ||
            p->count > disk->sectors - p->start || (p->status != 0 && p->status != 0x80))
            return -EINVAL;
        if (extended_type(p->type) && ++extended > 1)
            return -EINVAL;
        for (unsigned j = 0; j < i; j++)
            if (primary[j].type &&
                (uint64_t)p->start < (uint64_t)primary[j].start + primary[j].count &&
                (uint64_t)primary[j].start < (uint64_t)p->start + p->count)
                return -EINVAL;
    }
    for (unsigned i = 0; i < 4; i++) {
        struct mbr_entry *p = &primary[i];
        if (!p->type)
            continue;
        if (!extended_type(p->type)) {
            int r = stage_partition(disk, parts, count, i + 1, p->start, p->count, false);
            if (r)
                return r;
            continue;
        }
        uint64_t base = p->start, limit = base + p->count, at = base, seen[128];
        unsigned visited = 0;
        for (;;) {
            if (at < base || at >= limit || visited == 128)
                return -EINVAL;
            for (unsigned j = 0; j < visited; j++)
                if (seen[j] == at) {
                    kprintf("EBR cycle rejected\n");
                    return -ELOOP;
                }
            for (unsigned j = 0; j < *count; j++)
                if (at >= parts[j].start && at < parts[j].start + parts[j].count)
                    return -EINVAL;
            seen[visited++] = at;
            uint8_t buffer[512];
            int r = block_io(disk, at, 1, buffer, false);
            if (r)
                return r;
            if (buffer[510] != 0x55 || buffer[511] != 0xaa)
                return -EINVAL;
            struct mbr_entry e[4];
            memcpy(e, buffer + 446, sizeof(e));
            if (e[2].type || e[3].type || e[0].status > 0x80 ||
                (e[0].status != 0 && e[0].status != 0x80))
                return -EINVAL;
            if (e[0].type) {
                uint64_t first = at + e[0].start;
                if (extended_type(e[0].type) || !e[0].start || first < at || first >= limit ||
                    e[0].count > limit - first)
                    return -EINVAL;
                r = stage_partition(disk, parts, count, logical++, first, e[0].count, false);
                if (r)
                    return r;
            }
            if (!e[1].type)
                break;
            if (!extended_type(e[1].type) || e[1].status || e[1].start >= p->count || !e[1].count ||
                e[1].count > p->count - e[1].start)
                return -EINVAL;
            at = base + e[1].start;
        }
    }
    return 0;
}
int partitions_scan(struct blockdev *disk)
{
    uint8_t buffer[512];
    if (block_io(disk, 0, 1, buffer, false) || buffer[510] != 0x55 || buffer[511] != 0xaa) {
        kprintf("Partition: %s has no MBR signature\n", disk->name);
        return -EINVAL;
    }
    struct mbr_entry primary[4];
    memcpy(primary, buffer + 446, sizeof(primary));
    bool protective = false;
    for (unsigned i = 0; i < 4; i++)
        if (primary[i].type == 0xee && primary[i].start == 1 && primary[i].count)
            protective = true;
    struct partition parts[32];
    unsigned count = 0;
    int r = 0;
    if (protective) {
        struct gpt_header h;
        uint8_t *entries = NULL;
        r = gpt_read(disk, 1, &h, &entries);
        if (r) {
            kprintf("GPT primary invalid on %s (%d), checking backup\n", disk->name, r);
            r = gpt_read(disk, disk->sectors - 1, &h, &entries);
        }
        if (r) {
            kprintf("GPT rejected: %d\n", r);
            return r;
        }
        uint8_t zero[16] = {0};
        for (unsigned i = 0; i < h.count; i++) {
            struct gpt_entry e;
            memcpy(&e, entries + (uint64_t)i * h.entry_size, sizeof(e));
            if (!memcmp(e.type, zero, 16))
                continue;
            if (e.first < h.first || e.last > h.last || e.last < e.first ||
                !memcmp(e.guid, zero, 16)) {
                r = -EINVAL;
                break;
            }
            for (unsigned j = 0; j < i; j++) {
                struct gpt_entry previous;
                memcpy(&previous, entries + (uint64_t)j * h.entry_size, sizeof(previous));
                if (memcmp(previous.type, zero, 16) && !memcmp(previous.guid, e.guid, 16)) {
                    r = -EINVAL;
                    break;
                }
            }
            if (r)
                break;
            r = stage_partition(disk, parts, &count, i + 1, e.first, e.last - e.first + 1,
                                e.attributes & (1UL << 60));
            if (r)
                break;
        }
        kfree(entries);
    } else
        r = scan_mbr(disk, primary, parts, &count);
    if (r) {
        kprintf("Partition table %s rejected: %d\n", disk->name, r);
        return r;
    }
    publish_partitions(disk, parts, count);
    return 0;
}
