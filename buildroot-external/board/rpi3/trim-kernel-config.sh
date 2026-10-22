#!/bin/bash
# Regenerates board/rpi3/linux.config, the kernel config Buildroot uses:
#
#   bcm2711_defconfig + linux.fragment, minus the subsystems listed below,
#   minus every module (the image has no modprobe), minus module support.
#
# Run it from the repository root after a build exists (it needs the kernel
# tree and the cross compiler from output/), whenever the kernel version or
# linux.fragment changes:
#
#   buildroot-external/board/rpi3/trim-kernel-config.sh
#   make -C output linux-reconfigure all
set -e

BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-output}"
K="$OUT/build/linux-custom"
X="$(pwd)/$OUT/host/bin/aarch64-linux-"

km() { make -s -C "$K" ARCH=arm64 CROSS_COMPILE="$X" "$@"; }
off() { for s in "$@"; do sed -i -E "s/^CONFIG_$s=.*/# CONFIG_$s is not set/" "$K/.config"; done; }

km bcm2711_defconfig
(cd "$K" && ARCH=arm64 scripts/kconfig/merge_config.sh -m .config "$BOARD_DIR/linux.fragment" >/dev/null)
km olddefconfig

# Built-in subsystems this device has no use for: Pi 4 hardware, filesystems
# we never mount, containers/virtualisation/tracing, USB storage, swap...
# (GCC_PLUGINS: Buildroot turns it off anyway.)
off ARCH_BRCMSTB PCI BCMGENET \
	EXT4_FS F2FS_FS NFS_FS ROOT_NFS \
	VIRTUALIZATION KVM NETFILTER CGROUPS NAMESPACES BPF_SYSCALL AUDIT SECURITY KEYS \
	KPROBES FTRACE PROFILING \
	MD SCSI USB_STORAGE SWAP NUMA HID USB_HID LOGO WIRELESS \
	GCC_PLUGINS

# Drop every module; disabling some can make others appear, so repeat.
for i in 1 2 3 4 5; do
	sed -i -E 's/^CONFIG_([A-Za-z0-9_]+)=m$/# CONFIG_\1 is not set/' "$K/.config"
	km olddefconfig
	grep -q '=m$' "$K/.config" || break
done
if grep -q '=m$' "$K/.config"; then
	echo "modules still left in $K/.config" >&2
	exit 1
fi

# With no modules left, turning MODULES off can't promote anything to =y.
off MODULES
km olddefconfig

km savedefconfig
cp "$K/defconfig" "$BOARD_DIR/linux.config"
echo "wrote $BOARD_DIR/linux.config ($(grep -c '=y$' "$K/.config") built-in options)"
