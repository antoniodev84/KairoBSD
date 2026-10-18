# Third-party software

The KarioBSD operating system itself is licensed under the MIT License
(see LICENSE). This file lists third-party components bundled with or
used to build KarioBSD. Each component remains under its own license.

--------------------------------------------------------------------------------

## chibicc

Source: https://github.com/rui314/chibicc
Revision: `90d1f7f199cc55b13c7fdb5839d1409806633fdb`.
License: MIT. Full notice: `third_party/chibicc/LICENSE`.

The KarioBSD port changes the compiler driver, target macros, include and
library paths, process error handling, output checks, AMD64 variadic
register layout, long-double classification, and static-target assembly
syntax. The compiler uses KarioBSD system calls through libkario.
Upstream test entry points and non-license source comments have been
removed.

--------------------------------------------------------------------------------

## TinyCC assembler and linker

Source: https://github.com/TinyCC/tinycc
Revision: `43c7708b85681a2fd4451c8a541af4494a8919b2`.
License: LGPL 2.1 or later. See `third_party/tinycc/COPYING` and the
per-file notices retained in the source tree. All corresponding source
and KarioBSD modifications are included. The Makefile produces the object
files needed to rebuild and relink the native tools with a modified
libkario or a modified TinyCC.

The x86-64 assembler and ELF linker are exposed as /bin/as and /bin/ld
through user/toolchain/binutils.c. The source is specialized for the KarioBSD ELF64 x86-64 target. Inactive
Windows, Mach-O, other CPU targets, and the disabled in-process execution
runtime have been removed. Configuration disables dynamic host loading,
bounds instrumentation, and host semaphore dependencies. KarioBSD extensions implement local/common
symbols and the scalar SSE instructions emitted by chibicc. Target
executables are static ELF64 binaries. Upstream source comments other
than licensing notices have been removed.

Note on LGPL compliance: /bin/as and /bin/ld are distributed as part of
KarioBSD. Users may relink these binaries against a modified TinyCC
and/or a modified libkario using the KarioBSD build system:

    make build/root/bin/as build/root/bin/ld

The intermediate object files (third_party/tinycc/libtcc.o,
user/toolchain/binutils.o, and the libc objects) are produced by the
build and can be substituted.

### dwarf.h

File: `third_party/tinycc/dwarf.h`
Origin: elfutils (https://sourceware.org/elfutils/)
Copyright (C) 2000-2011, 2014, 2016, 2017, 2018 Red Hat, Inc.
License: LGPL 3 or later, or GPL 2 or later (dual-licensed, at your
option). This is a different license from the rest of TinyCC and from
KarioBSD itself.

This file is used only for DWARF debug information generation. If
KarioBSD is distributed in a form where users cannot replace this file
or rebuild the tools that include it (for example, a locked-down
appliance with verified boot), the anti-tivoization terms of LGPL 3 may
apply. For a freely redistributable operating system, this is not a
practical concern.

If you wish to remove this dependency entirely, replace this file with
an equivalent DWARF constant header under a permissive license, or
disable DWARF generation in the TinyCC build.

--------------------------------------------------------------------------------

## Limine

Source: https://github.com/limine-bootloader/limine
UEFI binaries: revision `aad3edd370955449717a334f0289dee10e2c5f01`,
`v8.x-binary` branch.
License: BSD 2-Clause. See `boot/limine/LICENSE`.

The native protocol header, boot/limine.h, retains its 0BSD license
notice.

--------------------------------------------------------------------------------

## Bitmap font

`kernel/drivers/font.h` derives from Daniel Hepper's public-domain
font8x8_basic, with attribution to Marcel Sondaar and IBM.

Source: https://github.com/dhepper/font8x8

--------------------------------------------------------------------------------

## External build and execution tools

Clang/LLVM, GNU ar, QEMU, OVMF, xorriso, Python, and e2fsprogs run on the
host. They are not linked into the KarioBSD kernel or libkario, and are
not redistributed by this repository (see tools/bootstrap.sh). Their
locally installed copies retain their own licenses.
