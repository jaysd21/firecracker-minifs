#!/usr/bin/env bash
# Starts a local NebulaR ddl_minifs_target_node (file-backed, UCX/TCP loopback),
# uploads a random image with minifs-populate and runs the live integration test.
# Uses NebulaR binaries read-only; all state lives under $WORK.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
NEBULAR_BUILD="${NebulaR_BUILD_DIR:-$HOME/nebula-ai/repos/NebulaR/build}"
TARGET="${TARGET_NODE:-$NEBULAR_BUILD/bin/tools/ddl_minifs_target_node}"
WORK="${WORK:-$(mktemp -d)}"
PORT="${PORT:-58123}"
OBJECT_SIZE="${OBJECT_SIZE:-262144}"
IMAGE_MB="${IMAGE_MB:-5}"

# UCX/TCP on "lo" can fail to connect (the stock ddl_minifs_client_node fails the
# same way in WSL2); use a real interface unless overridden.
IFACE="${IFACE:-$(ip -4 route get 1.1.1.1 | sed -n 's/.* dev \([^ ]*\).*/\1/p')}"
HOST_IP="${HOST_IP:-$(ip -4 -o addr show dev "$IFACE" | awk '{print $4}' | cut -d/ -f1 | head -1)}"
export UCX_TLS="${UCX_TLS:-tcp}" UCX_NET_DEVICES="${UCX_NET_DEVICES:-$IFACE}" UCX_CM_REUSEADDR=n NEBULA_BACKEND=posix
mkdir -p "$WORK/meta"
head -c $((IMAGE_MB * 1024 * 1024 - 1000)) /dev/urandom > "$WORK/image.raw"   # not a multiple of object size

"$TARGET" --bind "$HOST_IP" --port "$PORT" --ddl-cpu 0 --storage-cpu-offset 1 \
  --devices "$WORK/dev0.img" --device-size-gb 1 --metadata "$WORK/meta" --posix \
  --max-object-mb 1 --max-inflight 64 > "$WORK/target.log" 2>&1 &
TARGET_PID=$!
trap 'kill $TARGET_PID 2>/dev/null || true; wait $TARGET_PID 2>/dev/null || true' EXIT
for _ in $(seq 1 100); do
  grep -q -i "ready\|listening" "$WORK/target.log" 2>/dev/null && break
  kill -0 $TARGET_PID 2>/dev/null || { cat "$WORK/target.log"; echo "target died"; exit 1; }
  sleep 0.2
done
sleep 1

"$BUILD/minifs-populate" --image "$WORK/image.raw" --server "$HOST_IP" --port "$PORT" \
  --tenant 7 --object-hi 0x1111222233334444 --object-start 100 --object-size "$OBJECT_SIZE"

MINIFS_SERVER="$HOST_IP" MINIFS_PORT=$PORT MINIFS_TENANT=7 \
MINIFS_OBJECT_HI=0x1111222233334444 MINIFS_OBJECT_START=100 \
MINIFS_OBJECT_SIZE=$OBJECT_SIZE MINIFS_IMAGE="$WORK/image.raw" \
  "$BUILD/minifs_integration_test"
echo "integration test OK (work dir: $WORK)"
