#!/usr/bin/env bash
# Starts remote-disk-backend against an existing MiniFS target, boots a
# Firecracker microVM whose root device is that remote disk, and tears down.
#
# Required env:  SERVER  OBJECTS  KERNEL  FIRECRACKER
# Optional env:  PORT=58000 TENANT=1 OBJECT_HI=0xdd1c000000000000 OBJECT_START=1
#                OBJECT_SIZE=1048576 DDL_CPU=2 MEM_MIB=256 VCPUS=1
#                BOOT_ARGS (default boots /sbin/demo-init) WORK=$(mktemp -d)
#                UCX_TLS/UCX_NET_DEVICES (passed through to UCX)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BACKEND="${BACKEND:-$ROOT/build/remote-disk-backend}"
: "${SERVER:?}" "${OBJECTS:?}" "${KERNEL:?}" "${FIRECRACKER:?}"
PORT="${PORT:-58000}" TENANT="${TENANT:-1}" OBJECT_HI="${OBJECT_HI:-0xdd1c000000000000}"
OBJECT_START="${OBJECT_START:-1}" OBJECT_SIZE="${OBJECT_SIZE:-1048576}"
DDL_CPU="${DDL_CPU:-2}" MEM_MIB="${MEM_MIB:-256}" VCPUS="${VCPUS:-1}"
BOOT_ARGS="${BOOT_ARGS:-console=ttyS0 reboot=k panic=1 pci=off root=/dev/vda ro rootfstype=ext4 init=/sbin/demo-init}"
WORK="${WORK:-$(mktemp -d)}"; mkdir -p "$WORK"
SOCK="$WORK/vhost-user-blk.sock"
export UCX_CM_REUSEADDR=n

"$BACKEND" --server "$SERVER" --port "$PORT" --tenant "$TENANT" \
  --object-hi "$OBJECT_HI" --object-start "$OBJECT_START" --object-size "$OBJECT_SIZE" \
  --objects "$OBJECTS" --ddl-cpu "$DDL_CPU" --socket "$SOCK" > "$WORK/backend.log" 2>&1 &
BACKEND_PID=$!
cleanup() { kill -TERM "$BACKEND_PID" 2>/dev/null || true; wait "$BACKEND_PID" 2>/dev/null || true; }
trap cleanup EXIT
for _ in $(seq 1 300); do
  grep -q REMOTE_DISK_READY "$WORK/backend.log" && break
  kill -0 "$BACKEND_PID" 2>/dev/null || { cat "$WORK/backend.log"; echo "backend exited"; exit 1; }
  sleep 0.1
done
grep -q REMOTE_DISK_READY "$WORK/backend.log" || { cat "$WORK/backend.log"; echo "backend not ready"; exit 1; }

cat > "$WORK/vm.json" <<JSON
{
  "boot-source": {"kernel_image_path": "$KERNEL", "boot_args": "$BOOT_ARGS"},
  "drives": [{"drive_id": "rootfs", "is_root_device": true, "socket": "$SOCK"}],
  "machine-config": {"vcpu_count": $VCPUS, "mem_size_mib": $MEM_MIB}
}
JSON
rm -f "$WORK/fc.sock"
set +e
timeout --foreground "${VM_TIMEOUT:-120}" "$FIRECRACKER" --api-sock "$WORK/fc.sock" --config-file "$WORK/vm.json" \
  ${FC_ARGS:---no-seccomp} 2>&1 | tee "$WORK/vm.log"
FC_RC=${PIPESTATUS[0]}
set -e
kill -TERM "$BACKEND_PID" 2>/dev/null || true
wait "$BACKEND_PID" 2>/dev/null || true
trap - EXIT
echo "---- backend log ($WORK/backend.log)"
cat "$WORK/backend.log"
echo "firecracker exit code: $FC_RC (work dir: $WORK)"
exit "$FC_RC"
