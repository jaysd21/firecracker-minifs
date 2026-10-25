# Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `connect timed out` / UCX `no route to 200.x.x.x` | UCX/TCP loopback problem (also breaks NebulaR's own client in WSL2). Bind the target to a real interface IP and set `UCX_NET_DEVICES` to it on both sides. |
| `connect timed out` right after starting a new target | Target still formatting its device; wait for `[DDL_MINIFS_TARGET_READY]`. |
| Backend: `cannot listen on socket` | Stale/forbidden socket path; directory must exist and be writable. |
| Firecracker: error connecting to vhost-user socket | Backend not running yet, or path differs from the drive's `socket`. Start the backend first. |
| Firecracker: guest memory not shareable | Only if memfd shared memory could not be set up; Firecracker handles this when a vhost-user drive is configured. |
| Guest: `VFS: Cannot open root device` | Kernel lacks virtio-blk/ext4, wrong `root=`, or the image is not a raw filesystem (arbitrary benchmark objects are not bootable). |
| Guest I/O errors, backend logs `read failed: object not found` | Objects not populated for the whole range, or `--object-hi/--object-start/--tenant` differ from populate. Re-run populate; compare ids. |
| `read failed: timeout` / `disconnected` | Target slow or gone; raise `--timeout-seconds`; the library reconnects automatically. |
| `read failed: invalid request` | `--object-size` larger than the target's `--max-object-mb`, or differs from the size used when populating. |
| `short read` | Object stored smaller than `--object-size`; re-populate (it pads). |
| Backend exits with `storage did not drain` | Transport never returned in-flight GETs within 5 s; buffers are leaked deliberately. Check target health. |
| Snapshot request fails | Not supported by Firecracker with vhost-user devices. |
| Slow boot | Every read is a network GET with no cache; use a faster transport (RDMA via UCX env), larger `--max-inflight`, or add a cache layer. |

Debug aids: backend stderr (`vhost-user-blk: ...`), `[REMOTE_DISK_STATS]` at exit, Firecracker's `--log-path`,
and `scripts/integration_test.sh` to isolate storage from Firecracker.
