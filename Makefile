SMP ?= 2
MEMORY ?= 256M
ifeq ($(wildcard tools/host/usr/lib/llvm-19/bin/clang),)
CC := clang
LD := ld.lld
QEMU := qemu-system-x86_64
XORRISO := xorriso
OVMF ?= /usr/share/OVMF/OVMF_CODE_4M.fd
else
CC := tools/host-tool clang
LD := tools/host-tool ld.lld
QEMU := tools/host-tool qemu-system-x86_64
XORRISO := tools/host-tool xorriso
OVMF ?= tools/host/usr/share/OVMF/OVMF_CODE_4M.fd
endif
COMMON := --target=x86_64-unknown-none-elf -std=c17 -O2 -g -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -msoft-float -Wall -Wextra -Werror -Wno-unused-parameter -Wno-misleading-indentation -Ishared/include
KCFLAGS := $(COMMON) -mcmodel=kernel -Ikernel/include -Iboot
UCFLAGS := $(filter-out -mno-mmx -mno-sse -mno-sse2 -msoft-float,$(COMMON)) -Ilibc/include
KCS := $(shell find kernel -name '*.c')
KAS := $(shell find kernel -name '*.S')
KOBJ := $(patsubst %.c,build/%.o,$(KCS)) $(patsubst %.S,build/%.o,$(KAS))
LCS := $(wildcard libc/src/*.c)
LOBJ := $(patsubst %.c,build/%.o,$(LCS)) build/libc/src/start.o build/libc/src/setjmp.o
BINS := echo cat ls pwd mkdir rm uname meminfo uptime ln chmod mount stat cp sleep true false partscan kill
CHICS := $(wildcard third_party/chibicc/*.c)
CHIOBJ := $(patsubst %.c,build/%.o,$(CHICS))
TCFLAGS := $(filter-out -Werror,$(UCFLAGS)) -Wno-sign-compare -Wno-unused-function -Wno-switch -Wno-missing-field-initializers -Ithird_party/tinycc
USER := build/root/bin/cc build/root/bin/chibicc build/root/bin/as build/root/bin/ld build/root/sbin/init build/root/bin/sh $(addprefix build/root/bin/,$(BINS))
.PHONY: all clean iso run debug
all: build/kernel.elf build/initramfs.tar
build/kernel.elf: $(KOBJ) boot/kernel.ld
	$(LD) -nostdlib -static -z max-page-size=4096 -T boot/kernel.ld $(KOBJ) -o $@
build/kernel/%.o: kernel/%.c
	@mkdir -p $(@D)
	$(CC) $(KCFLAGS) -MMD -MP -c $< -o $@
build/kernel/%.o: kernel/%.S
	@mkdir -p $(@D)
	$(CC) --target=x86_64-unknown-none-elf -g -c $< -o $@
build/libc/%.o: libc/%.c
	@mkdir -p $(@D)
	$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
build/libc/src/%.o: libc/src/%.S
	@mkdir -p $(@D)
	$(CC) --target=x86_64-unknown-none-elf -g -c $< -o $@
build/user/%.o: user/%.c
	@mkdir -p $(@D)
	$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@
build/root/sbin/init: build/user/init/main.o $(LOBJ) libc/user.ld
	@mkdir -p $(@D)
	$(LD) -nostdlib -static -z max-page-size=4096 -T libc/user.ld $(LOBJ) $< -o $@
build/root/bin/sh: build/user/shell/main.o $(LOBJ) libc/user.ld
	@mkdir -p $(@D)
	$(LD) -nostdlib -static -z max-page-size=4096 -T libc/user.ld $(LOBJ) $< -o $@
build/root/bin/%: build/user/bin/main.o $(LOBJ) libc/user.ld
	@mkdir -p $(@D)
	$(LD) -nostdlib -static -z max-page-size=4096 -T libc/user.ld $(LOBJ) $< -o $@
build/third_party/chibicc/%.o: third_party/chibicc/%.c
	@mkdir -p $(@D)
	$(CC) $(filter-out -Werror,$(UCFLAGS)) -Wno-sign-compare -Wno-unused-function -Wno-switch -Wno-missing-field-initializers -MMD -MP -c $< -o $@
build/third_party/tinycc/libtcc.o: third_party/tinycc/libtcc.c
	@mkdir -p $(@D)
	$(CC) $(TCFLAGS) -MMD -MP -c $< -o $@
build/user/toolchain/binutils.o: user/toolchain/binutils.c
	@mkdir -p $(@D)
	$(CC) $(TCFLAGS) -MMD -MP -c $< -o $@
build/root/bin/cc: build/root/bin/chibicc
	cp $< $@
build/root/bin/chibicc: $(CHIOBJ) $(LOBJ) libc/user.ld
	@mkdir -p $(@D)
	$(LD) -nostdlib -static -z max-page-size=4096 -T libc/user.ld $(LOBJ) $(CHIOBJ) -o $@
build/root/bin/as build/root/bin/ld: build/user/toolchain/binutils.o build/third_party/tinycc/libtcc.o $(LOBJ) libc/user.ld
	@mkdir -p $(@D)
	$(LD) -nostdlib -static -z max-page-size=4096 -T libc/user.ld $(LOBJ) build/user/toolchain/binutils.o build/third_party/tinycc/libtcc.o -o $@
build/sysroot.stamp: $(LOBJ) $(shell find libc/include shared/include third_party/chibicc/include -type f) third_party/chibicc/LICENSE third_party/tinycc/COPYING boot/limine/LICENSE tools/sysroot.py
	python3 tools/sysroot.py
	@touch $@
build/initramfs.tar: $(USER) build/sysroot.stamp tools/initramfs.py
	python3 tools/initramfs.py build/root $@
iso: build/kariobsd.iso
build/kariobsd.iso: all boot/limine.conf boot/limine/BOOTX64.EFI boot/limine/limine-uefi-cd.bin
	mkdir -p build/iso/boot build/iso/EFI/BOOT
	cp build/kernel.elf build/initramfs.tar boot/limine.conf build/iso/boot/
	cp boot/limine/BOOTX64.EFI build/iso/EFI/BOOT/
	cp boot/limine/limine-uefi-cd.bin build/iso/boot/
	$(XORRISO) -as mkisofs -R -J -e boot/limine-uefi-cd.bin -no-emul-boot -efi-boot-part --efi-boot-image --protective-msdos-label build/iso -o $@
build/virtio.img build/sata.img:
	@mkdir -p build
	python3 tools/disk-image.py $@
run: iso build/virtio.img build/sata.img
	$(QEMU) -machine q35 -cpu max -m $(MEMORY) -smp $(SMP) -drive if=pflash,format=raw,readonly=on,file=$(OVMF) -cdrom build/kariobsd.iso -drive if=none,id=vblk,file=build/virtio.img,format=raw -device virtio-blk-pci,drive=vblk,disable-legacy=on -drive if=none,id=sata,file=build/sata.img,format=raw -device ide-hd,drive=sata,bus=ide.0 -serial mon:stdio -display none -nic none -no-reboot
debug: iso build/virtio.img build/sata.img
	$(QEMU) -machine q35 -cpu max -m $(MEMORY) -smp $(SMP) -drive if=pflash,format=raw,readonly=on,file=$(OVMF) -cdrom build/kariobsd.iso -drive if=none,id=vblk,file=build/virtio.img,format=raw -device virtio-blk-pci,drive=vblk,disable-legacy=on -drive if=none,id=sata,file=build/sata.img,format=raw -device ide-hd,drive=sata,bus=ide.0 -serial mon:stdio -display none -nic none -no-reboot -S -s
clean:
	rm -rf build
-include $(KOBJ:.o=.d) $(LOBJ:.o=.d) build/user/init/main.d build/user/shell/main.d build/user/bin/main.d

-include $(CHIOBJ:.o=.d) build/third_party/tinycc/libtcc.d build/user/toolchain/binutils.d
