# KarioBSD

A small x86-64 monolithic operating system, bootable via UEFI.

## Features

- UEFI boot through Limine, 4-level paging, NX, CR0.WP
- Preemptive SMP scheduler with fork/exec/exit and POSIX signals
- VFS with tmpfs, devfs, and read/write ext2
- Drivers: serial console (framebuffer + font8x8), APIC/IOAPIC, LAPIC timer, PCIe (ECAM), AHCI, virtio-blk
- libkario: syscall wrappers, stdio, stdlib, string, signals, time
- Userland: init, sh (job control, pipelines, redirections), coreutils
- Self-hosting toolchain: chibicc (`/bin/cc`, `/bin/chibicc`) and TinyCC (`/bin/as`, `/bin/ld`)

## Building

Requires Clang/LLD, QEMU, OVMF, xorriso, Python 3, and e2fsprogs on the host.

```sh
make          # build/kernel.elf and build/initramfs.tar
make iso      # build/kariobsd.iso
make run      # boot in QEMU
make debug    # boot in QEMU with GDB stub (-S -s)
```

Optional: tools/bootstrap.sh downloads the host toolchain into tools/host.

Tunables: SMP=N (default 2), MEMORY=256M.

## Layout

```
boot/        Limine config, linker script, limine.h
kernel/      arch, mm, sched, sys, fs, drivers, ipc, acpi
libc/        libkario
user/        init, shell, bin, toolchain
shared/      kario.h (ABI)
third_party/ chibicc, TinyCC
tools/       build scripts
```

## License

MIT. See LICENSE.

Bundled third-party components retain their own licenses — notably TinyCC
(LGPL 2.1+) and elfutils' dwarf.h (LGPL 3+/GPL 2+). See
third_party.md
