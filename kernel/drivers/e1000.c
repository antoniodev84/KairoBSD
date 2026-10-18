#include "../net/net.h"
#define E1000_RING_COUNT 32
struct e1000_rx {
    uint64_t address;
    uint16_t length, checksum;
    uint8_t status, errors;
    uint16_t special;
} PACKED;
struct e1000_tx {
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset, command, status, checksum_start;
    uint16_t special;
} PACKED;
struct e1000 {
    struct net_card card;
    volatile uint32_t *registers;
    uint64_t receive_dma, transmit_dma;
    struct e1000_rx *receive;
    struct e1000_tx *transmit;
    unsigned receive_index, transmit_index;
    bool discard;
};
static uint32_t e1000_read(struct e1000 *d, unsigned reg)
{
    return d->registers[reg / 4];
}
static void e1000_write(struct e1000 *d, unsigned reg, uint32_t value)
{
    d->registers[reg / 4] = value;
    (void)d->registers[8 / 4];
}
static int e1000_transmit(struct net_card *card, const void *data, size_t length)
{
    struct e1000 *d = card->driver;
    if (length > 1514)
        return -EMSGSIZE;
    unsigned next = (d->transmit_index + 1) % E1000_RING_COUNT;
    if (next == e1000_read(d, 0x3810))
        return -EAGAIN;
    struct e1000_tx *descriptor = &d->transmit[d->transmit_index];
    if (!(*(volatile uint8_t *)&descriptor->status & 1))
        return -EAGAIN;
    memcpy(phys_ptr(descriptor->address), data, length);
    descriptor->length = length;
    descriptor->command = 0x0b;
    descriptor->status = 0;
    atomic_thread_fence(memory_order_release);
    d->transmit_index = next;
    e1000_write(d, 0x3818, d->transmit_index);
    return 0;
}
static void e1000_poll(struct net_card *card)
{
    struct e1000 *d = card->driver;
    (void)e1000_read(d, 0xc0);
    for (unsigned budget = 0; budget < E1000_RING_COUNT; budget++) {
        unsigned index = d->receive_index;
        struct e1000_rx *descriptor = &d->receive[index];
        uint8_t status = *(volatile uint8_t *)&descriptor->status;
        if (!(status & 1))
            break;
        atomic_thread_fence(memory_order_acquire);
        bool complete = status & 2;
        if (!d->discard && complete && !descriptor->errors && descriptor->length >= 14 &&
            descriptor->length <= 1514)
            net_receive(card, phys_ptr(descriptor->address), descriptor->length);
        d->discard = !complete;
        descriptor->status = 0;
        atomic_thread_fence(memory_order_release);
        e1000_write(d, 0x2818, index);
        d->receive_index = (index + 1) % E1000_RING_COUNT;
    }
    if (e1000_read(d, 8) & 2)
        card->link = true;
    else
        card->link = false;
}
static void e1000_release(struct e1000 *d)
{
    if (d->receive)
        for (unsigned i = 0; i < E1000_RING_COUNT; i++)
            if (d->receive[i].address)
                dma_free(d->receive[i].address, 1);
    if (d->transmit)
        for (unsigned i = 0; i < E1000_RING_COUNT; i++)
            if (d->transmit[i].address)
                dma_free(d->transmit[i].address, 1);
    if (d->receive_dma)
        dma_free(d->receive_dma, 1);
    if (d->transmit_dma)
        dma_free(d->transmit_dma, 1);
    kfree(d);
}
int e1000_probe(struct pci_device *pci)
{
    if ((pci_read(pci, 16, 4) & 1) || pci->bar_size[0] < 0x20000)
        return -ENODEV;
    struct e1000 *d = kzalloc(sizeof(*d));
    if (!d)
        return -ENOMEM;
    d->registers = mmio_map(pci->bar[0], pci->bar_size[0]);
    if (!d->registers) {
        kfree(d);
        return -ENOMEM;
    }
    uint16_t command = pci_read(pci, 4, 2);
    pci_write(pci, 4, 2, command | 6 | (1U << 10));
    e1000_write(d, 0xd8, UINT32_MAX);
    e1000_write(d, 0x100, 0);
    e1000_write(d, 0x400, 0);
    delay_ns(10000000);
    e1000_write(d, 0, e1000_read(d, 0) | (1U << 26));
    uint64_t deadline = monotonic_ns() + 100000000;
    while (e1000_read(d, 0) & (1U << 26)) {
        if (monotonic_ns() >= deadline) {
            pci_write(pci, 4, 2, command);
            kfree(d);
            return -ETIMEDOUT;
        }
        pause_cpu();
    }
    delay_ns(20000000);
    e1000_write(d, 0xd8, UINT32_MAX);
    (void)e1000_read(d, 0xc0);
    uint32_t low = e1000_read(d, 0x5400), high = e1000_read(d, 0x5404);
    for (unsigned i = 0; i < 4; i++)
        d->card.mac[i] = low >> (i * 8);
    d->card.mac[4] = high;
    d->card.mac[5] = high >> 8;
    if (!(high & (1U << 31)) || (d->card.mac[0] & 1)) {
        pci_write(pci, 4, 2, command);
        kfree(d);
        return -ENODEV;
    }
    d->receive_dma = dma_alloc(1, UINT64_MAX);
    d->transmit_dma = dma_alloc(1, UINT64_MAX);
    if (!d->receive_dma || !d->transmit_dma)
        goto memory_error;
    d->receive = phys_ptr(d->receive_dma);
    d->transmit = phys_ptr(d->transmit_dma);
    for (unsigned i = 0; i < E1000_RING_COUNT; i++) {
        d->receive[i].address = dma_alloc(1, UINT64_MAX);
        d->transmit[i].address = dma_alloc(1, UINT64_MAX);
        d->transmit[i].status = 1;
        if (!d->receive[i].address || !d->transmit[i].address)
            goto memory_error;
    }
    for (unsigned i = 0; i < 128; i++)
        e1000_write(d, 0x5200 + i * 4, 0);
    e1000_write(d, 0x2800, d->receive_dma);
    e1000_write(d, 0x2804, d->receive_dma >> 32);
    e1000_write(d, 0x2808, E1000_RING_COUNT * sizeof(*d->receive));
    e1000_write(d, 0x2810, 0);
    e1000_write(d, 0x2818, E1000_RING_COUNT - 1);
    e1000_write(d, 0x3800, d->transmit_dma);
    e1000_write(d, 0x3804, d->transmit_dma >> 32);
    e1000_write(d, 0x3808, E1000_RING_COUNT * sizeof(*d->transmit));
    e1000_write(d, 0x3810, 0);
    e1000_write(d, 0x3818, 0);
    e1000_write(d, 0x410, 0x0060200a);
    e1000_write(d, 0x3828, (1U << 24) | (1U << 16));
    e1000_write(d, 0x2828, (1U << 24) | (1U << 16));
    e1000_write(d, 0, (e1000_read(d, 0) | (1U << 6)) & ~((1U << 3) | (1U << 31)));
    e1000_write(d, 0x100, (1U << 1) | (1U << 15) | (1U << 26));
    e1000_write(d, 0x400, (1U << 1) | (1U << 3) | (15U << 4) | (64U << 12));
    d->card.driver = d;
    d->card.transmit = e1000_transmit;
    d->card.poll = e1000_poll;
    int result = net_register(&d->card);
    if (!result)
        return 0;
    e1000_write(d, 0x100, 0);
    e1000_write(d, 0x400, 0);
    pci_write(pci, 4, 2, command);
    e1000_release(d);
    return result;
memory_error:
    pci_write(pci, 4, 2, command);
    e1000_release(d);
    return -ENOMEM;
}
