#!/usr/bin/env bash
# End-to-end read-only boot demo on one machine:
#   download Firecracker + kernel + Ubuntu rootfs -> build ext4 image -> start a
#   local NebulaR MiniFS target -> upload the image to objects -> boot the VM from
#   the remote disk and check the guest's output.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEMO="${DEMO_DIR:-$HOME/fc-demo}"
FC_VER="${FC_VER:-v1.17.0}"; CI_VER="${CI_VER:-v1.15}"; KVER="${KVER:-6.1.155}"
NEBULAR_BUILD="${NebulaR_BUILD_DIR:-$HOME/nebula-ai/repos/NebulaR/build}"
TARGET="${TARGET_NODE:-$NEBULAR_BUILD/bin/tools/ddl_minifs_target_node}"
PORT="${PORT:-58200}"; OBJECT_HI=0xfc00000000000000; OBJECT_SIZE=1048576
IFACE="${IFACE:-$(ip -4 route get 1.1.1.1 | sed -n 's/.* dev \([^ ]*\).*/\1/p')}"
HOST_IP="${HOST_IP:-$(ip -4 -o addr show dev "$IFACE" | awk '{print $4}' | cut -d/ -f1 | head -1)}"
export UCX_TLS="${UCX_TLS:-tcp}" UCX_NET_DEVICES="${UCX_NET_DEVICES:-$IFACE}" UCX_CM_REUSEADDR=n NEBULA_BACKEND=posix
mkdir -p "$DEMO/dl" "$DEMO/run"

FC="$DEMO/dl/release-$FC_VER-x86_64/firecracker-$FC_VER-x86_64"
if [ ! -x "$FC" ]; then
  curl -fsSL -o "$DEMO/dl/fc.tgz" "https://github.com/firecracker-microvm/firecracker/releases/download/$FC_VER/firecracker-$FC_VER-x86_64.tgz"
  tar -C "$DEMO/dl" -xzf "$DEMO/dl/fc.tgz"
fi
S3=https://s3.amazonaws.com/spec.ccfc.min/firecracker-ci/$CI_VER/x86_64
[ -f "$DEMO/dl/vmlinux-$KVER" ] || curl -fsSL -o "$DEMO/dl/vmlinux-$KVER" "$S3/vmlinux-$KVER"
[ -f "$DEMO/dl/ubuntu-24.04.squashfs" ] || curl -fsSL -o "$DEMO/dl/ubuntu-24.04.squashfs" "$S3/ubuntu-24.04.squashfs"
[ -f "$DEMO/rootfs.ext4" ] || "$ROOT/scripts/make_rootfs.sh" "$DEMO/dl/ubuntu-24.04.squashfs" "$DEMO/rootfs.ext4" 1024

# Local MiniFS target (file-backed device). Startup formats the device: ~30s.
rm -rf "$DEMO/run/meta" "$DEMO/run/dev0.img"; mkdir -p "$DEMO/run/meta"
"$TARGET" --bind "$HOST_IP" --port "$PORT" --ddl-cpu 0 --storage-cpu-offset 1 \
  --devices "$DEMO/run/dev0.img" --device-size-gb 3 --metadata "$DEMO/run/meta" --posix \
  --max-object-mb 1 --max-inflight 64 > "$DEMO/run/target.log" 2>&1 &
TARGET_PID=$!
trap 'kill $TARGET_PID 2>/dev/null || true; wait $TARGET_PID 2>/dev/null || true' EXIT
for _ in $(seq 1 600); do
  grep -q DDL_MINIFS_TARGET_READY "$DEMO/run/target.log" && break
  kill -0 $TARGET_PID 2>/dev/null || { tail "$DEMO/run/target.log"; echo "target died"; exit 1; }
  sleep 0.5
done
grep -q DDL_MINIFS_TARGET_READY "$DEMO/run/target.log" || { echo "target not ready"; exit 1; }

"$ROOT/build/minifs-populate" --image "$DEMO/rootfs.ext4" --server "$HOST_IP" --port "$PORT" \
  --tenant 1 --object-hi $OBJECT_HI --object-start 1 --object-size $OBJECT_SIZE --timeout-seconds 60
OBJECTS=$(( ($(stat -c %s "$DEMO/rootfs.ext4") + OBJECT_SIZE - 1) / OBJECT_SIZE ))

SERVER="$HOST_IP" PORT=$PORT TENANT=1 OBJECT_HI=$OBJECT_HI OBJECT_START=1 OBJECT_SIZE=$OBJECT_SIZE \
OBJECTS=$OBJECTS KERNEL="$DEMO/dl/vmlinux-$KVER" FIRECRACKER="$FC" WORK="$DEMO/run/vm" \
  "$ROOT/scripts/run_microvm.sh" | tee "$DEMO/run/demo.out"

expected=$(debugfs -R "cat /usr/bin/bash" "$DEMO/rootfs.ext4" 2>/dev/null | sha256sum | cut -d' ' -f1)
grep -q "REMOTE_ROOTFS_BOOT_OK" "$DEMO/run/demo.out" || { echo "FAIL: guest did not report success"; exit 1; }
grep -q "sha256 of /bin/bash: $expected" "$DEMO/run/demo.out" || { echo "FAIL: guest checksum differs from image ($expected)"; exit 1; }
echo "DEMO PASSED: guest read /bin/bash from the remote disk, sha256 matches the image ($expected)"
