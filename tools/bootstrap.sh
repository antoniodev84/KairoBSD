#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$base"
mkdir -p tools/apt/lists/partial tools/apt/cache/archives/partial tools/host
apt-get -o Dir::State::lists="$base/tools/apt/lists" -o Dir::Cache="$base/tools/apt/cache" -o Debug::NoLocking=true -o APT::Sandbox::User="$(id -un)" update
apt-get -y --download-only --no-install-recommends -o Dir::State::lists="$base/tools/apt/lists" -o Dir::Cache="$base/tools/apt/cache" -o Debug::NoLocking=true -o APT::Sandbox::User="$(id -un)" install clang-19 lld-19 clang-format-19 qemu-system-x86 ovmf xorriso e2fsprogs
(
    cd tools/apt/cache/archives
    apt-get -o Dir::State::lists="$base/tools/apt/lists" -o Debug::NoLocking=true download clang-19 lld-19 clang-format-19 qemu-system-x86 qemu-system-common qemu-system-data ovmf xorriso e2fsprogs seabios
)
for package in tools/apt/cache/archives/*.deb; do
    dpkg-deb -x "$package" tools/host
done
if [ -e tools/host/usr/share/seabios/vgabios-stdvga.bin ]; then
    ln -sf ../seabios/vgabios-stdvga.bin tools/host/usr/share/qemu/vgabios-stdvga.bin
fi
tools/host-tool clang --version
tools/host-tool qemu-system-x86_64 --version
