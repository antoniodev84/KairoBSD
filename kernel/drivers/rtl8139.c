#include "../net/net.h"
#define RTL_RING_SIZE 8192
struct rtl8139 {
    struct net_card card;
    uint16_t port, receive_offset;
    unsigned transmit_index;
    uint64_t receive_dma, transmit_dma;
};
static uint16_t rtl_read16(struct rtl8139 *d, unsigned reg)
{
    uint16_t value;
    uint16_t port = d->port + reg;
    __asm__ volatile("inw %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}
static void rtl_write16(struct rtl8139 *d, unsigned reg, uint16_t value)
{
    uint16_t port = d->port + reg;
    __asm__ volatile("outw %0,%1" ::"a"(value), "Nd"(port));
}
static void rtl_start(struct rtl8139 *d)
{
    outl(d->port + 0x30, d->receive_dma);
    outb(d->port + 0x37, 0x0c);
    outl(d->port + 0x44, 0x0000e70a);
    outl(d->port + 0x40, 0x03000700);
    rtl_write16(d, 0x38, 0xfff0);
    d->receive_offset = 0;
    for (unsigned i = 0; i < 4; i++)
        outl(d->port + 0x20 + i * 4, d->transmit_dma + i * 2048);
}
static int rtl_transmit(struct net_card *card, const void *data, size_t length)
{
    struct rtl8139 *d = card->driver;
    if (length > 1514)
        return -EMSGSIZE;
    unsigned index = d->transmit_index;
    uint32_t status = inl(d->port + 0x10 + index * 4);
    if (!(status & (1U << 13)))
        return -EAGAIN;
    uint8_t *buffer = phys_ptr(d->transmit_dma + index * 2048);
    memcpy(buffer, data, length);
    if (length < 60) {
        memset(buffer + length, 0, 60 - length);
        length = 60;
    }
    atomic_thread_fence(memory_order_release);
    outl(d->port + 0x10 + index * 4, (uint32_t)length);
    d->transmit_index = (index + 1) % 4;
    return 0;
}
static void rtl_poll(struct net_card *card)
{
    struct rtl8139 *d = card->driver;
    uint16_t interrupt = rtl_read16(d, 0x3e);
    if (interrupt & 0x50) {
        outb(d->port + 0x37, 0x04);
        rtl_start(d);
    }
    rtl_write16(d, 0x3e, interrupt);
    uint8_t *ring = phys_ptr(d->receive_dma);
    for (unsigned budget = 0; budget < 64 && !(inb(d->port + 0x37) & 1); budget++) {
        unsigned at = d->receive_offset;
        uint16_t status = ring[at] | ((uint16_t)ring[(at + 1) % RTL_RING_SIZE] << 8);
        uint16_t length =
            ring[(at + 2) % RTL_RING_SIZE] | ((uint16_t)ring[(at + 3) % RTL_RING_SIZE] << 8);
        if (length == 0xfff0)
            break;
        if (!(status & 1) || (status & 0x3e) || length < 18 || length > 1518) {
            outb(d->port + 0x37, 0x04);
            rtl_start(d);
            break;
        }
        uint8_t packet[1514];
        for (unsigned i = 0; i < (unsigned)length - 4; i++)
            packet[i] = ring[(at + 4 + i) % RTL_RING_SIZE];
        d->receive_offset = ALIGN_UP(at + length + 4, 4) % RTL_RING_SIZE;
        rtl_write16(d, 0x38, d->receive_offset - 16);
        net_receive(card, packet, length - 4);
    }
    bool link = !(inb(d->port + 0x58) & 4);
    if (link)
        card->link = true;
    else
        card->link = false;
}
int rtl8139_probe(struct pci_device *pci)
{
    unsigned bar;
    for (bar = 0; bar < 6; bar++)
        if ((pci_read(pci, 16 + bar * 4, 4) & 1) && pci->bar_size[bar] >= 0x100)
            break;
    if (bar == 6 || pci->bar[bar] > 0xff00)
        return -ENODEV;
    struct rtl8139 *d = kzalloc(sizeof(*d));
    if (!d)
        return -ENOMEM;
    d->port = pci->bar[bar];
    d->receive_dma = dma_alloc(3, 1UL << 32);
    d->transmit_dma = dma_alloc(2, 1UL << 32);
    if (!d->receive_dma || !d->transmit_dma)
        goto memory_error;
    uint16_t command = pci_read(pci, 4, 2);
    pci_write(pci, 4, 2, command | 5 | (1U << 10));
    outb(d->port + 0x52, 0);
    outb(d->port + 0x37, 0x10);
    uint64_t deadline = monotonic_ns() + 100000000;
    while (inb(d->port + 0x37) & 0x10) {
        if (monotonic_ns() >= deadline) {
            pci_write(pci, 4, 2, command);
            dma_free(d->receive_dma, 3);
            dma_free(d->transmit_dma, 2);
            kfree(d);
            return -ETIMEDOUT;
        }
        pause_cpu();
    }
    rtl_write16(d, 0x3c, 0);
    for (unsigned i = 0; i < 6; i++)
        d->card.mac[i] = inb(d->port + i);
    d->card.driver = d;
    d->card.transmit = rtl_transmit;
    d->card.poll = rtl_poll;
    rtl_start(d);
    int result = net_register(&d->card);
    if (!result)
        return 0;
    outb(d->port + 0x37, 0);
    pci_write(pci, 4, 2, command);
    dma_free(d->receive_dma, 3);
    dma_free(d->transmit_dma, 2);
    kfree(d);
    return result;
memory_error:
    if (d->receive_dma)
        dma_free(d->receive_dma, 3);
    if (d->transmit_dma)
        dma_free(d->transmit_dma, 2);
    kfree(d);
    return -ENOMEM;
}
