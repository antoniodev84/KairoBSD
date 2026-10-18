#include <kernel.h>
uint64_t boot_epoch;
uint64_t realtime_seconds(void)
{
    return boot_epoch + monotonic_ns() / 1000000000UL;
}
__attribute__((used,
               section(".limine_requests_start"))) static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests"))) static volatile LIMINE_BASE_REVISION(2);
#define REQUEST __attribute__((used, section(".limine_requests")))
REQUEST static volatile struct limine_boot_time_request time_request = {
    .id = LIMINE_BOOT_TIME_REQUEST};
REQUEST static volatile struct limine_hhdm_request hhdm_request = {.id = LIMINE_HHDM_REQUEST};
REQUEST static volatile struct limine_memmap_request memory_request = {.id = LIMINE_MEMMAP_REQUEST};
REQUEST static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST};
REQUEST static volatile struct limine_rsdp_request rsdp_request = {.id = LIMINE_RSDP_REQUEST};
REQUEST static volatile struct limine_smp_request smp_request = {.id = LIMINE_SMP_REQUEST};
REQUEST static volatile struct limine_module_request module_request = {.id = LIMINE_MODULE_REQUEST};
REQUEST static volatile struct limine_firmware_type_request firmware_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST};
REQUEST static volatile struct limine_stack_size_request stack_request = {
    .id = LIMINE_STACK_SIZE_REQUEST, .stack_size = 65536};
REQUEST static volatile struct limine_paging_mode_request paging_request = {
    .id = LIMINE_PAGING_MODE_REQUEST, .mode = LIMINE_PAGING_MODE_X86_64_4LVL};
__attribute__((used, section(".limine_requests_end"))) static volatile LIMINE_REQUESTS_END_MARKER;
NORETURN void kmain(void)
{
    __asm__ volatile("cli");
    serial_init();
    if (!LIMINE_BASE_REVISION_SUPPORTED || !hhdm_request.response || !memory_request.response ||
        !module_request.response || !module_request.response->module_count ||
        !rsdp_request.response || !smp_request.response || !firmware_request.response)
        panic("Required Limine response missing");
    if (firmware_request.response->firmware_type != LIMINE_FIRMWARE_TYPE_UEFI64)
        panic("KarioBSD requires UEFI64");
    if (!framebuffer_request.response || !framebuffer_request.response->framebuffer_count)
        panic("UEFI framebuffer missing");
    console_init(framebuffer_request.response->framebuffers[0]);
    kprintf(
        "\nKarioBSD %s\nArchitecture: x86_64\nFirmware: UEFI\nKernel: KarioBSD Monolithic Kernel\n",
        KARIO_VERSION);
    uint64_t efer = rdmsr(0xc0000080);
    if (!(efer & (1U << 10)))
        panic("Long mode not active");
    if (!paging_request.response || paging_request.response->mode != LIMINE_PAGING_MODE_X86_64_4LVL)
        panic("Four-level paging response required");
    if (!memory_request.response->entry_count || memory_request.response->entry_count > 4096)
        panic("Invalid memory map entry count");
    struct limine_file *initramfs = module_request.response->modules[0];
    if (!initramfs || !initramfs->address || initramfs->size < 1024 || initramfs->size > SIZE_MAX)
        panic("Invalid initramfs module");
    hhdm = hhdm_request.response->offset;
    if (hhdm < 0xffff800000000000UL || hhdm % PAGE_SIZE)
        panic("Invalid HHDM response");
    kprintf("Initializing memory...\n");
    pmm_init(memory_request.response);
    vmm_init();
    idt_init();
    arch_init(&cpus[0]);
    kprintf("Initializing ACPI...\n");
    acpi_init((uint64_t)rsdp_request.response->address);
    kprintf("Initializing APIC...\n");
    apic_init();
    timer_init();
    if (time_request.response && time_request.response->boot_time > 0)
        boot_epoch = time_request.response->boot_time;
    kprintf("Discovering PCIe and block devices...\n");
    net_init();
    pci_init();
    unsigned disks = block_count;
    for (unsigned i = 0; i < disks; i++)
        partitions_scan(&blocks[i]);
    kprintf("Mounting root filesystem...\n");
    struct limine_file *module = module_request.response->modules[0];
    vfs_init(module->address, module->size);
    struct vnode *mnt = tmpfs_new(S_IFDIR | 0755);
    if (!mnt || vfs_mount("/mnt", vfs_root, mnt))
        panic("Mount directory initialization");
    vnode_put(mnt);
    for (unsigned i = 0; i < block_count; i++)
        if (blocks[i].parent) {
            struct vnode *root;
            int r = ext2_mount(&blocks[i], &root);
            if (!r) {
                char path[PATH_MAX];
                snprintf(path, sizeof(path), "/mnt/%s", blocks[i].name);
                struct vnode *point;
                r = vfs_create(path, vfs_root, S_IFDIR | 0755, NULL, &point);
                if (!r) {
                    vnode_put(point);
                    r = vfs_mount(path, vfs_root, root);
                }
                vnode_put(root);
                if (r)
                    panic("Disk mount failed %d", r);
            } else
                kprintf("Filesystem probe %s: %d\n", blocks[i].name, r);
        }
    kprintf("Initializing scheduler...\n");
    sched_init();
    if (!task_kernel(net_worker, 0))
        panic("Network worker allocation failed");
    const char *args[] = {"/sbin/init", NULL};
    const char *env[] = {"PATH=/bin", "HOME=/", "TERM=kario", NULL};
    int pid = task_spawn("/sbin/init", NULL, args, env);
    if (pid < 0)
        panic("Loading /sbin/init: %d", pid);
    smp_start(smp_request.response);
    serial_irq_init();
    timer_cpu_init();
    kprintf("Starting /sbin/init (PID %d), %u CPUs...\n", pid, cpu_count);
    struct frame boot = {0};
    arch_enter(schedule(&boot, false));
}
