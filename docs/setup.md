# Setup

## 1. Build the project

See the README. Binaries land in `build/`: `remote-disk-backend`, `minifs-populate`.

## 2. NebulaR side

Build NebulaR's client/target with UCX (`-DNEBULA_ENABLE_UCX=ON`); you need `libnebula_ddl3.a` etc. in the
build tree passed as `-DNebulaR_BUILD_DIR`, and a running DDL3 target, e.g.:

```bash
export UCX_TLS=tcp UCX_NET_DEVICES=eth0 UCX_CM_REUSEADDR=n NEBULA_BACKEND=posix
ddl_minifs_target_node --bind <ip> --port 58000 --ddl-cpu 0 --storage-cpu-offset 1 \
  --devices <dev-or-file> [--device-size-gb N] --metadata <dir> [--posix] \
  --max-object-mb 1 --max-inflight 64
```

Runtime: the target and every client need the same UCX transport settings. Wait for `[DDL_MINIFS_TARGET_READY]`
(formatting a new device takes ~30 s for 3 GB). UCX on `lo` failed to connect in WSL2 even for NebulaR's own
`ddl_minifs_client_node`; bind to a real interface address.

## 3. Prepare a raw disk image

Any raw image the guest can boot, e.g. an ext4 root filesystem. Example (no root needed):

```bash
scripts/make_rootfs.sh ubuntu-24.04.squashfs rootfs.ext4 1024   # adds /sbin/demo-init
```

The guest kernel must have virtio-blk and ext4 built in (Firecracker CI kernels do). With `root=/dev/vda ro`.

## 4. Populate the object range

```bash
build/minifs-populate --image rootfs.ext4 --server <ip> --port 58000 --tenant 1 \
  --object-hi 0xfc00000000000000 --object-start 1 --object-size 1048576
```

Uploads object `start+i` = bytes `[i*size,(i+1)*size)` (last one zero-padded) and reads each back to verify.
`object-size` must be a multiple of 512 and ≤ the target's `--max-object-mb`. Choose a distinct
(tenant, object-hi, start) per image.

## 5. Start the backend

```bash
build/remote-disk-backend --server <ip> --port 58000 --tenant 1 \
  --object-hi 0xfc00000000000000 --object-start 1 --object-size 1048576 \
  --objects 1024 --ddl-cpu 2 --socket /run/fc/rootfs.sock
```

Prints `[REMOTE_DISK_READY]` with the capacity. `--disk-bytes` exports less than `objects*size`.
Options: `--numa`, `--max-inflight`, `--timeout-seconds`. SIGINT/SIGTERM shuts down and prints `[REMOTE_DISK_STATS]`.
Use a `--ddl-cpu` distinct from the target's CPUs when co-located.

## 6. Configure Firecracker (v1.17.0)

Config-file form (`firecracker --config-file vm.json --api-sock fc.sock`):

```json
{
  "boot-source": {"kernel_image_path": "vmlinux-6.1.155",
    "boot_args": "console=ttyS0 reboot=k panic=1 pci=off root=/dev/vda ro rootfstype=ext4"},
  "drives": [{"drive_id": "rootfs", "is_root_device": true, "socket": "/run/fc/rootfs.sock"}],
  "machine-config": {"vcpu_count": 1, "mem_size_mib": 256}
}
```

or over the API: `PUT /drives/rootfs {"drive_id":"rootfs","is_root_device":true,"socket":"/run/fc/rootfs.sock"}`.
Start the backend first; Firecracker connects when the drive is configured. Add `init=/sbin/demo-init` for the demo image.

## 7–8. Boot and verify

`scripts/run_microvm.sh` does 5–8 for an existing server (env: `SERVER OBJECTS KERNEL FIRECRACKER`, see header).
`scripts/demo.sh` does everything locally. Verified output on this machine (Firecracker v1.17.0):

```
[DEMO] root mount:   /dev/root / ext4 ro,relatime 0 0
[DEMO] block device: 2097152 sectors, ro=1
[DEMO] os-release:   Ubuntu 24.04.3 LTS
[DEMO] sha256 of /bin/bash: bc5945fe...   (matches debugfs of the image)
[DEMO] write test: touch: cannot touch '/should-fail': Read-only file system
[REMOTE_DISK_STATS] requests=915 failed=0 chunk_gets=924 bytes_read=11142144 retries=0
```

i.e. ~11 MB read of the 1 GiB image; nothing was downloaded up front.

## Tests

* `ctest --test-dir build` – 42 tests: mapping (boundaries, overflow, bad layouts), `RemoteDisk` with a mock
  (spanning reads, out-of-order completion, missing object, GET failure, partial failure, backpressure, retries,
  shutdown, exactly-once, buffer lifetime, read-only) and the vhost-user server against a fake Firecracker-style frontend.
* `scripts/integration_test.sh` – local MiniFS target + populate + 5 live tests (data equality, random concurrent
  reads, missing object, partial failure, range checks).
