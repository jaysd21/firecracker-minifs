#!/usr/bin/env bash
# Builds a raw ext4 root filesystem image from a Firecracker CI Ubuntu squashfs
# without root (fakeroot). Adds /sbin/demo-init, which proves the guest can read
# its remote root filesystem, prints a checksum, and powers the VM off.
#   make_rootfs.sh ubuntu-24.04.squashfs out.ext4 [size_mb]
set -euo pipefail
SQUASH="$1"; OUT="$2"; SIZE_MB="${3:-1024}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fakeroot -- bash -euo pipefail -c '
  unsquashfs -no-progress -d "$0/root" "$1" >/dev/null
  cat > "$0/root/sbin/demo-init" <<INIT
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null
echo "[DEMO] guest kernel: \$(uname -r)"
echo "[DEMO] root mount:   \$(grep " / " /proc/mounts | head -1)"
echo "[DEMO] block device: \$(cat /sys/block/vda/size) sectors, ro=\$(cat /sys/block/vda/ro)"
. /etc/os-release; echo "[DEMO] os-release:   \$PRETTY_NAME"
set -- \$(sha256sum /bin/bash); echo "[DEMO] sha256 of /bin/bash: \$1"
echo "[DEMO] write test: \$( (touch /should-fail) 2>&1 | head -1 )"
echo "[DEMO] REMOTE_ROOTFS_BOOT_OK"
reboot -f
INIT
  chmod 755 "$0/root/sbin/demo-init"
  rm -f "$2"
  truncate -s "${3}M" "$2"
  mkfs.ext4 -q -F -O ^has_journal -d "$0/root" "$2"
' "$WORK" "$SQUASH" "$OUT" "$SIZE_MB"
e2fsck -fn "$OUT" | tail -1
echo "rootfs image: $OUT"
