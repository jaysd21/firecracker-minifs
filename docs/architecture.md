# Architecture

## Layers (all in this repo unless noted)

| Layer | File | Role |
|---|---|---|
| vhost-user-blk server | `src/vhost_user_blk.cc` | Speaks the vhost-user protocol, walks split virtqueues, owns the event loop |
| `RemoteDisk` | `src/remote_disk.cc` | Splits reads into per-object chunks, bounded queues, retries, exactly-once completion |
| `DiskMapping` | `src/disk_mapping.cc` | Pure offset → (object id, offset) arithmetic with overflow checks |
| `StorageClient` | `include/remote_disk/storage_client.h` | Async GET interface; implemented by `MockStorage` (tests) and `MiniFsAdapter` |
| `MiniFsAdapter` | `src/minifs_adapter.cc` | The only code that touches `CoreClient` |
| `nebula_ddl3` (NebulaR) | external | DDL3 over UCX |

## Storage semantics (verified against NebulaR sources and a live target)

* **Partial GET works.** `ddl_minifs_target.cc` accepts `DdlMiniFsReadRequest{offset,length,0}` with
  `0 < length <= max_object_bytes` and `offset+length` not overflowing, and returns exactly `length` bytes.
* **Missing object → `CoreStatus::kNotFound`** (live test: "object not found").
* **Object size ≤ server `--max-object-mb`** (default 1 MiB), for both PUT and GET. Reads past the end of an
  object are not part of the contract, so the image is zero-padded to whole objects by `minifs-populate`.
* **Objects must be created first** (PUT). Object id = `(object_id_hi, start + index)` within a tenant.
  The backend never PUTs.
* **Mutability:** a PUT replaces an object, so nothing stops someone changing objects under a running VM.
  The base image is treated as immutable by convention; there is no versioning or lease.

Disk layout: object `start+i` holds disk bytes `[i*object_size, (i+1)*object_size)`.
Capacity = `object_count * object_size`, or `--disk-bytes` if smaller.

## Threading

`CoreClient`'s ring is single-producer/single-consumer and `poll()` must run on the owner core, so the
whole backend is one thread: it pins to `--ddl-cpu`, creates the client, and runs one loop that
`epoll`s the vhost-user socket and kick eventfds, and calls `RemoteDisk::pump()` (busy-polling while I/O is
in flight, blocking up to 100 ms when idle).

## Request life cycle and guarantees

1. Guest kicks; the server parses the descriptor chain (direct or indirect) into segments, allocates a
   **bounce buffer owned by the request**, and calls `RemoteDisk::read`.
2. `read` validates against capacity (rejecting overflow), splits into extents, queues them.
   Returns `kBusy` when `max_requests` (64) are already admitted: the server leaves the avail entry
   unconsumed and retries after completions (backpressure to the guest).
3. `pump` submits up to `max_inflight_chunks` (8) GETs, each writing directly into the bounce buffer.
   `kQueueFull` from the ring or server is retried (≤ `max_retries`); other errors fail the request and cancel its
   unsent chunks. A GET returning fewer bytes than asked is `kShortRead`.
4. When every submitted chunk has completed the callback runs **once**, never from inside `read`:
   the server copies bounce → guest memory, writes the virtio status (`IOERR` on any error), fills the used ring and
   signals the call eventfd. Out-of-order completion is fine.
5. Shutdown/disconnect: queued chunks fail with `kShutdown`; in-flight chunks are awaited (5 s). If the
   transport never returns them the guest requests are still completed once, the buffers are leaked on purpose and
   the process exits non-zero, since freeing memory UCX may still write to would be worse.
6. Frontend disconnect: the queue is stopped, I/O drained, guest memory unmapped, and the backend waits for a
   new Firecracker connection.

Timeouts and connection loss come from the library (`request_timeout_ns`, reconnect with back-off): pending requests
complete with `kTimeout` / `kDisconnected` and surface as virtio `IOERR`.

## Firecracker v1.17.0 compatibility (from its source)

* Drive config: `PUT /drives/{id}` with `socket`, `is_root_device`; one socket per device.
* Frontend requests `VIRTIO_F_VERSION_1`, `RING_F_EVENT_IDX`, `VHOST_USER_F_PROTOCOL_FEATURES`, `BLK_F_RO` and
  protocol feature `CONFIG`; the guest is offered the intersection with the backend. This backend offers
  VERSION_1, PROTOCOL_FEATURES, BLK_F_RO (plus BLK_SIZE, INDIRECT_DESC which Firecracker does not request). It
  does not offer EVENT_IDX or FLUSH.
* **`CONFIG` is mandatory in practice**: capacity comes from `GET_CONFIG` (60 bytes). REPLY_ACK is implemented.
* Read-only is decided by the backend (`BLK_F_RO`). Queue count 1, size 256.
* Init order handled: SET_OWNER, GET/SET_FEATURES, GET/SET_PROTOCOL_FEATURES, GET_CONFIG, SET_MEM_TABLE (memfd per
  region), SET_VRING_NUM/ADDR/BASE/CALL/KICK/ENABLE.
* Firecracker limitations: developer preview, no snapshots, no reset, hotplug only if booted with a vhost-user device,
  no rate limiter, shared-memfd guest memory slows page faults, jailer does not sandbox the backend.

## Next: writable overlays

Do not PUT whole objects for partial writes. Plan: keep the base image read-only and add a per-VM overlay
(copy-on-write at a fixed block size, e.g. 64 KiB, stored in a *separate* object range or local file) with a
persistent block bitmap; reads consult the bitmap, writes go to the overlay only. Needed: write path in
`StorageClient` (PUT of whole overlay blocks with read-modify-write for sub-block writes, serialized per block),
virtio `FLUSH` that waits for outstanding PUTs and a bitmap commit, crash-consistent ordering (data before bitmap),
and exclusive ownership of an overlay so two VMs never write the same one.
