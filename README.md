# KarioBSD

A small x86-64 monolithic operating system with UEFI boot, its own kernel, its own libc (`libkario`), and a self-hosting userland.

## Features

- UEFI boot via Limine, 4-level paging, NX and CR0.WP
- Preemptive SMP scheduler with `fork`/`exec`/`exit` and POSIX signals
- Real job control: process groups, sessions, `tcsetpgrp`, `SIGTSTP`/`SIGCONT`/`SIGTTIN`/`SIGTTOU`
- VFS with tmpfs, devfs, and read/write ext2
- Drivers: serial + framebuffer console, APIC/IOAPIC, LAPIC timer, PCIe (ECAM), AHCI, virtio-blk, RTL8139, e1000
- Native networking: Ethernet, ARP, IPv4, ICMP, UDP, TCP, loopback, fragmentation and reassembly, DHCPv4 with renewal and rebinding
- DNS resolver with cache, CNAME chains, UDP retry, and TCP fallback
- `wget` and `httpd` over native sockets
- Userland: init, shell with job control, coreutils, ifconfig, ping, nc, nslookup
- Self-hosting toolchain: chibicc (`/bin/cc`, `/bin/chibicc`) and TinyCC (`/bin/as`, `/bin/ld`)

## Building

Requires Clang/LLD, QEMU, OVMF, xorriso, Python 3, and e2fsprogs on the host.

```sh
make          # kernel.elf and initramfs.tar
make iso      # ISO image
make run      # boot in QEMU
make debug    # QEMU with GDB (-S -s)
```

Tunables: SMP=N (default 2), MEMORY=256M, NET_MODEL=e1000e|rtl8139.
tools/bootstrap.sh downloads the host toolchain into tools/host.

# Layout

```
boot/        Limine config, linker script, limine.h
kernel/      arch, mm, sched, sys, fs, drivers, ipc, acpi, net
libc/        libkario
user/        init, shell, bin, toolchain, network, http, os
shared/      kario.h (syscall ABI)
third_party/ chibicc, TinyCC
tools/       build scripts
```

# Usage

```sh
ifconfig
ifconfig en0 dhcp
ping localhost
nslookup example.com
wget http://HOST:8080/file
httpd -f -b 0.0.0.0 -p 8080 -h /tmp/www
```

Port forwarding to reach the guest server from the host:

```sh
make run NET_ARGS='-netdev user,id=net0,hostfwd=tcp:127.0.0.1:8080-:8080 -device e1000e,netdev=net0'
```

## chroot and partition install

```sh
chroot NEWROOT [COMMAND [ARG...]]
os-install /dev/sd0p1
os-init /dev/sd0p1
```

os-install copies /bin, /sbin, /etc, /usr, verifies contents, and writes a completion marker. os-init validates the installation, mounts devfs in the target, chroots, and runs /bin/sh. It does not install a bootloader or format the partition.

## License

MIT. See LICENSE. Bundled third-party components retain their own licenses — see third_party.md.
