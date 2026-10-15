#include <kernel.h>
#include "font.h"
static struct limine_framebuffer *fb;
static unsigned col, row, cols, rows;
static unsigned char input[1024];
static atomic_uint input_head, input_tail;
static bool irq_input;
static spinlock_t input_lock = SPINLOCK_INIT;
static spinlock_t lock = SPINLOCK_INIT;
void serial_init(void)
{
    outb(0x3f9, 0);
    outb(0x3fb, 0x80);
    outb(0x3f8, 1);
    outb(0x3f9, 0);
    outb(0x3fb, 3);
    outb(0x3fa, 0xc7);
    outb(0x3fc, 0x0b);
}
int serial_get(void)
{
    if (!irq_input)
        return (inb(0x3fd) & 1) ? inb(0x3f8) : -1;
    uint64_t flags = irq_save();
    spin_lock(&input_lock);
    unsigned tail = atomic_load_explicit(&input_tail, memory_order_relaxed),
             head = atomic_load_explicit(&input_head, memory_order_acquire);
    int result = -1;
    if (tail != head) {
        result = input[tail % sizeof(input)];
        atomic_store_explicit(&input_tail, tail + 1, memory_order_release);
    }
    spin_unlock(&input_lock);
    irq_restore(flags);
    return result;
}
void serial_irq(void)
{
    while (inb(0x3fd) & 1) {
        unsigned head = atomic_load_explicit(&input_head, memory_order_relaxed),
                 tail = atomic_load_explicit(&input_tail, memory_order_acquire);
        unsigned char byte = inb(0x3f8);
        if (byte == 3 || byte == 26 || byte == 28) {
            terminal_signal(byte == 3 ? SIGINT : byte == 26 ? SIGTSTP : SIGQUIT);
            continue;
        }
        if (head - tail < sizeof(input)) {
            input[head % sizeof(input)] = byte;
            atomic_store_explicit(&input_head, head + 1, memory_order_release);
        }
    }
    (void)inb(0x3fa);
}
void serial_irq_init(void)
{
    bool low, level;
    unsigned gsi = acpi_irq_gsi(4, &low, &level);
    if (!ioapic_route(gsi, 36, cpus[0].apic_id, low, level)) {
        irq_input = true;
        outb(0x3f9, 1);
        kprintf("UART: RX interrupt routed through I/O APIC GSI %u\n", gsi);
    }
}
static void serial_put(char c)
{
    while (!(inb(0x3fd) & 0x20))
        pause_cpu();
    outb(0x3f8, c);
}
void console_init(struct limine_framebuffer *f)
{
    if (f && f->bpp == 32 && f->memory_model == LIMINE_FRAMEBUFFER_RGB &&
        f->pitch >= f->width * 4) {
        fb = f;
        cols = f->width / 8;
        rows = f->height / 16;
        memset(fb->address, 0, fb->pitch * fb->height);
    }
}
static void scroll(void)
{
    if (row < rows)
        return;
    memmove(fb->address, (char *)fb->address + fb->pitch * 16, fb->pitch * (fb->height - 16));
    memset((char *)fb->address + fb->pitch * (fb->height - 16), 0, fb->pitch * 16);
    row = rows - 1;
}
static void glyph(unsigned c)
{
    if (!fb || !cols || !rows)
        return;
    if (c == '\n') {
        col = 0;
        row++;
        scroll();
        return;
    }
    if (c == '\r') {
        col = 0;
        return;
    }
    if (c == '\b' || c == 127) {
        if (col)
            col--;
        else if (row) {
            row--;
            col = cols - 1;
        }
        c = ' ';
    }
    if (c == '\t') {
        unsigned n = 8 - (col % 8);
        while (n--)
            glyph(' ');
        return;
    }
    if (c < 32 || c > 126)
        return;
    uint32_t fg = (0xd0U << fb->red_mask_shift) | (0xe0U << fb->green_mask_shift) |
                  (0xe0U << fb->blue_mask_shift);
    for (unsigned y = 0; y < 16; y++) {
        uint32_t *p = (void *)((uint8_t *)fb->address + (row * 16 + y) * fb->pitch + col * 32);
        for (unsigned x = 0; x < 8; x++)
            p[x] = (font[c][y / 2] & (1U << x)) ? fg : 0;
    }
    if (++col == cols) {
        col = 0;
        row++;
        scroll();
    }
}
void console_write(const char *s, size_t n)
{
    uint64_t f = irq_save();
    spin_lock(&lock);
    while (n--) {
        char c = *s++;
        if (c == '\n')
            serial_put('\r');
        serial_put(c);
        glyph((unsigned char)c);
    }
    spin_unlock(&lock);
    irq_restore(f);
}

void console_emergency(const char *s, size_t n)
{
    while (n--) {
        char c = *s++;
        if (c == '\n')
            serial_put('\r');
        serial_put(c);
    }
}
