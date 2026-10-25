# firecracker-minifs

Boot a [Firecracker](https://github.com/firecracker-microvm/firecracker) microVM from a
**read-only virtual disk stored as objects on a NebulaR MiniFS server**, without
downloading the image first. A userspace vhost-user block backend translates guest
reads into partial-object GETs over NebulaR's existing DDL3/UCX client (`nebula_ddl3`).

```
Firecracker ──vhost-user (Unix socket)──▶ remote-disk-backend ──DDL3/UCX──▶ NebulaR MiniFS target
  virtio-blk                               VhostUserBlk → RemoteDisk
                                           → DiskMapping → MiniFsAdapter(CoreClient)
```

No kernel driver, NBD or FUSE. NebulaR is an external dependency and is never modified.
Verified: Firecracker v1.17.0, Linux guest 6.1.155, Ubuntu 24.04 root filesystem; the
guest read ~11 MB of a 1 GiB image (see [docs/setup.md](docs/setup.md)).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNebulaR_ROOT="$HOME/nebula-ai/repos/NebulaR"
cmake --build build -j
ctest --test-dir build          # unit tests, no NebulaR server needed
```

Requirements: C++20 compiler, CMake ≥ 3.20, GoogleTest, UCX (`pkg-config ucx`), libnuma, and a
**NebulaR build tree** containing `libnebula_ddl3.a` (see [Dependency on NebulaR](#dependency-on-nebular)).
`-DWITH_NEBULA=OFF` builds only the storage-independent core and the unit tests.

## Targets

| Target | Purpose |
|---|---|
| `remote-disk-backend` | vhost-user-blk server Firecracker connects to |
| `minifs-populate` | provisioning tool: uploads a raw image into consecutive objects (not used at VM runtime) |
| `unit_tests` | mapping, async disk (mock storage) and vhost-user protocol tests |
| `minifs_integration_test` | live test against a real MiniFS target (`scripts/integration_test.sh`) |

## Quick start

```bash
scripts/demo.sh        # downloads Firecracker+kernel+rootfs, starts a local MiniFS target,
                       # uploads the image, boots the VM from it, checks the guest output
```

Diagrams: [architecture](docs/diagrams/architecture.html) and [guest read sequence](docs/diagrams/sequence.html) (open in a browser).

Manual flow and every option: [docs/setup.md](docs/setup.md). Design: [docs/architecture.md](docs/architecture.md).
Problems: [docs/troubleshooting.md](docs/troubleshooting.md).

## Dependency on NebulaR

Used API: `ddl::runtime::CoreClient` / `CoreServiceConfig` / `DdlSqe` / `DdlCqe` and
`nebula::objstore::DdlMiniFsReadRequest` / `kDdlMiniFsGetOpcode` / `kDdlMiniFsPutOpcode`
(`populate` only). Only `src/minifs_adapter.cc` and `tools/minifs_populate.cc` include NebulaR headers.

**Packaging gap (not worked around inside NebulaR):** NebulaR declares an export for
`Nebula::nebula_ddl3` in `common/CMakeLists.txt`, but configuring that standalone fails:
`install(EXPORT "NebulaCommonTargets") includes target "nebula_common" which requires target
"nebula_resiliency" that is not in any export set`. So there is no installable package today.
`cmake/FindNebulaDdl3.cmake` therefore (1) uses an installed `nebula_common` package if one
ever exists, otherwise (2) reads headers from `NebulaR_ROOT` and static libraries
(`nebula_ddl3 nebula_common nebula_resiliency nebula_log nebula_logring`) from
`NebulaR_BUILD_DIR` (default `$NebulaR_ROOT/build`), plus UCX and libnuma. Nothing is copied
or built inside NebulaR's tree. Suggested upstream fix: add `nebula_resiliency` to
`_NEBULA_COMMON_EXPORT_TARGETS` and install its headers, which would enable path (1).

## Limitations

* Read-only; writes/flush are rejected. Writable overlays: [architecture.md](docs/architecture.md#next-writable-overlays).
* Firecracker vhost-user-blk is a developer preview: no snapshots, no device reset, no rate limiter,
  guest memory must be memfd-shared (Firecracker does this automatically).
* One queue, one frontend at a time. Request data is bounced through a per-request buffer (no zero copy).
* The image must be uploaded into fixed-size objects of ≤ the server's `--max-object-mb` (default 1 MiB).
* No client-side cache: every guest read is a network GET. Each guest request waits for the
  library's request timeout (`--timeout-seconds`) if the server stalls.
