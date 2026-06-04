# [RFC]: Master-Side LOCAL_DISK Replica Warm Re-Adoption Across Client Restart

## Summary

When a Mooncake client (e.g., vLLM PD instance using ModeA) restarts, the master expires it after `client_live_ttl_sec` (default 10 s) and **immediately erases** all of its segment registrations and `LOCAL_DISK` replica metadata. The on-disk SSD files survive but become master-side invisible. The new client must rediscover everything via `FileStorage::ScanMeta` — and for the default OffsetAllocator backend, today's `ScanMeta` is in-memory only (`storage_backend.cpp:3124`) so it recovers nothing at all until PR #2215 lands. On the other backends, recovery is slow but possible (see §Motivation). Prefix-cache hit rate drops to ~0% during the gap.

This RFC introduces:

1. A new `LocalDiskReplicaState` enum (`OK → DISCONNECTED → REMOVED`) carried in-place on every LOCAL_DISK replica, with a grace timer instead of immediate erase. A returning client transitions a `DISCONNECTED` replica back to `OK` atomically.
2. A `local_disk_segment_id` bound to an on-SSD marker file at `${storage_path}/.mooncake_local_disk_segment_id` that identifies the storage location (independent of the per-process random `client_id`).

A returning client whose marker file matches a `DISCONNECTED` segment is auto-adopted via the existing `MountLocalDiskSegment` RPC. No `ScanMeta` walk needed.

**Scope** — what this RFC does and does *not* enable:


| Scenario                                                        | Today                                     | With this RFC                                                                                           | Fixed? |
| --------------------------------------------------------------- | ----------------------------------------- | ------------------------------------------------------------------------------------------------------- | ------ |
| A restarts; A's next request after restart                      | misses for the recovery window (see §Motivation) | hits immediately (sub-second)                                                                           | ✅      |
| Master state correctness across A's restart                     | transient zero-replica window             | continuous                                                                                              | ✅      |
| Other client B reading A's keys *while A is dead*               | clean miss                                | **still a clean miss** (A's process is the RDMA endpoint)                                               | ❌      |
| DRAM survives restart                                           | lost                                      | **still lost** (separate RFC track: graceful shutdown)                                                  | ❌      |
| Cross-instance availability when A is permanently lost          | data lost                                 | **still lost** (use `replica_num >= 2` for this)                                                        | ❌      |
| Deployment using `DISK` (3FS/shared FS) instead of `LOCAL_DISK` | **not affected by the bug**               | **no change** — `DiskReplicaData` has no `client_id`; client lifecycle is decoupled from data lifecycle | ➖      |


This RFC is scoped to `LOCAL_DISK` because `DiskReplicaData` doesn't have the owner-coupling problem — `file_path` is independent of any client, so `CleanupStaleHandles` never touches DISK replicas when a client dies. Operators who want to avoid this bug entirely by using a shared-FS offload tier can do so today; the trade-off is read latency (3FS network roundtrip vs LOCAL_DISK's microsecond RDMA). See Alternative I.

The fix is independent: works for all SSD backends, works for graceful and SIGKILL exits, and ships valuable on its own. Composes with PR #2215 and RFC #1920's Client Graceful Shutdown work.

## Motivation

The Mooncake paper ([arXiv:2407.00079](https://arxiv.org/abs/2407.00079) §4.2, Table 1) reports from Moonshot/Kimi's production trace: block size **512 tokens**, mean input length **7,590 tokens**, prefix cache hit rate climbing from **30% at 1K blocks to 51% at ∞**, with a heavy hot-block skew (a small set accessed 10K+ times).

When a client/PD instance restarts, today's master treats its `LOCAL_DISK` replicas as gone. The data is still on the SSD, but invisible — readers miss, and the next prefill recomputes the full prompt. The lost cache is concentrated value (the hot blocks); a cold restart drops the hit rate toward 0 until traffic re-warms it, inflating TTFT for a workload where prefill already dominates.

Recovery behavior varies sharply by SSD backend:


| Backend         | Recovery on client restart today                                                  |
| --------------- | --------------------------------------------------------------------------------- |
| OffsetAllocator | **Never** — `ScanMeta` is in-memory only (`storage_backend.cpp:3124`, V1); PR #2215 fixes this with snapshot-based recovery, but the master-side gap remains until ingest completes |
| FilePerKey      | Minutes — `ScanMeta` walks every file and reads its content (`storage_backend.cpp:1149`)                                                |
| Bucket          | Seconds to ~1 min — loads bucket-metadata files at Init (`storage_backend.cpp:1564`), then iterates in-memory                            |


Even with PR #2215, OffsetAllocator's recovery window is bounded below by `ClientMonitorFunc` TTL (10s default) + snapshot deserialization + `NotifyOffloadSuccess` ingest — measured in tens of seconds to minutes depending on cache size. This RFC closes that window to a single adoption RPC.

The goal of this RFC is to keep that data visible across the restart so the master can re-adopt it instead of waiting for a full re-ingest.

## Goals

- **G1** Eliminate the master-side metadata gap for `LOCAL_DISK` replicas across client restart on all SSD backends.
- **G2** Same-client post-restart hit rate ≥95% within sub-second of `MountLocalDiskSegment` (today: 0% during the per-backend recovery window described in §Motivation).
- **G3** Keep random `client_id` as the per-process identity; persistent identity is data-bound (marker file).
- **G4** Configurable grace window, defaulting conservatively to 10 min.
- **G5** Compose cleanly with PR #2215, RFC #1920, and Client Graceful Shutdown work.

## Non-Goals

- **DRAM recovery** across restart, tracked in RFC #1920's Client Graceful Shutdown track.
- **Cross-instance reads during downtime** — requires `replica_num >= 2` or shared DISK tier (3FS).
- **Live migration** of LOCAL_DISK to a different host.
- **Replica rebuild from peer** on permanent loss — KV cache data is recomputable.
- **HA durability of DISCONNECTED state** — deferred. The state is in-memory only; failover-safety comes from the seeded `client_ttl` mechanism (§6), which is correctness-safe but means a worst-case `client_live_ttl_sec + disconnect_grace_period_sec` grace window across a failover instead of `disconnect_grace_period_sec`. A future PR can persist DISCONNECTED via snapshot schema bump or etcd sidecar once broader rolling-upgrade work (RFC #1920) makes the migration cheap.

## Background

Today's `LOCAL_DISK` lifecycle is an implicit two-state model: live → erased. `ClientMonitorFunc` (`master_service.cpp:4914`) sees no Ping for 10s → calls `ClearInvalidHandles` → `CleanupStaleHandles` (`master_service.cpp:2530`) erases every replica whose owner client_id is no longer alive. No grace period.

Key paths to know:


| Path                            | Location                                                                    |
| ------------------------------- | --------------------------------------------------------------------------- |
| Random `client_id` per process  | `client_service.cpp:60`                                                     |
| Default 10s TTL                 | `types.h:95`                                                                |
| Expiry detection                | `master_service.cpp:4914`                                                   |
| Stale-handle erasure            | `master_service.cpp:2530`                                                   |
| Existing add-replica path       | `master_service.cpp:2706` (`NotifyOffloadSuccess`)                          |
| Existing remount path           | `master_service.cpp:351` (`ReMountSegment`)                                 |
| Reader visibility predicate     | `replica.h:248` (`fn_is_completed`)                                         |
| Master lock order               | `master_service.h:53-57` (client_mutex → shard mutex → segment_mutex)       |
| Existing delayed-delete pattern | `master_service.h:1402` (`DiscardedReplicas`, for write-path timeouts only) |


**What's new vs. existing patterns**: the proposed `LocalDiskReplicaState` is *orthogonal* to `ReplicaStatus`. `ReplicaStatus` describes the data's write lifecycle (`COMPLETE` = bytes are written); the new state describes the owner's liveness lifecycle (`DISCONNECTED` = owner is currently unreachable, may return). A replica can be `COMPLETE`+`DISCONNECTED` simultaneously. The delayed-delete pattern from `DiscardedReplicas` is the conceptual ancestor (TTL on cleanup), but this RFC annotates the replica *in place* rather than moving it to a separate list — necessary because adoption needs the per-key linkage.

## Design

### 1. State machine

```
        NotifyOffloadSuccess
                │
                ▼
           ┌─────────┐
           │   OK    │ ◄────────────────┐
           └────┬────┘                  │
   client       │                       │
   expires      │           returning client matches
                ▼           local_disk_segment_id;
           ┌──────────────┐ atomic transition under shard lock
           │ DISCONNECTED │ ─────────────┘
           │ (grace TTL)  │
           └──────┬───────┘
                  │ grace TTL expires
                  ▼
           ┌─────────┐
           │ REMOVED │ (master forgets entirely; entry erased by reaper)
           └─────────┘
```

`DISCONNECTED` is invisible to readers (see §3). The `DISCONNECTED → OK` transition (when a returning client with matching identity calls `MountLocalDiskSegment`) is atomic under the shard mutex — readers never see a half-adopted state.

### 2. `LocalDiskDescriptor` and the runtime sidecar

`LocalDiskDescriptor`'s serialized form is unchanged. All new runtime state lives in an in-memory sidecar on `MasterService`:

```cpp
enum class LocalDiskReplicaState {
    OK,            // owner alive and heartbeating
    DISCONNECTED,  // owner unreachable; grace timer running; awaiting same-identity reattach
    REMOVED        // grace expired; data unclaimable (entry erased by reaper)
};

struct LocalDiskRuntime {
    UUID local_disk_segment_id;          // from marker file (§4); identity for adoption
    LocalDiskReplicaState state{OK};
    std::optional<std::chrono::system_clock::time_point> grace_expiry;
};

// In MasterService:
std::unordered_map<ReplicaId, LocalDiskRuntime> local_disk_runtime_;
std::unordered_map<UUID, std::vector<ReplicaId>> local_disk_segment_id_index_;
```

- `local_disk_runtime_` carries per-replica liveness state. Lookup by `ReplicaId`.
- `local_disk_segment_id_index_` lets `MountLocalDiskSegment` find replicas to adopt in O(1) by marker UUID.
- Both are populated as `NotifyOffloadSuccess` writes LOCAL_DISK replicas and as `MountLocalDiskSegment` mounts segments. Both are cleared on `Reset()` (role transition). Neither is serialized.

Trade-off: adoption that coincides with a master leader failover degrades to FreshMount (the new leader doesn't have the index entry). The client then runs `ScanMeta` to re-ingest — today's slow path. This is the explicit cost of keeping snapshot wire-format unchanged in this RFC; it's recovered in a future PR if/when broader rolling-upgrade work makes the schema bump cheap.

### 3. Reader visibility

Add `fn_is_visible_to_reader`:

```cpp
[[nodiscard]] static bool fn_is_visible_to_reader(const Replica& r) {
    if (!r.is_completed()) return false;
    if (r.is_local_disk_replica()) {
        return r.get_descriptor().get_local_disk_descriptor().state
                   == LocalDiskReplicaState::OK;
    }
    return true;
}
```

`GetReplicaList` (`master_service.cpp:842`) switches from `fn_is_completed` to `fn_is_visible_to_reader`. The predicate is essential: extending replica lifetime past client death (to enable adoption) means a reader could otherwise be handed a `transport_endpoint` pointing at a dead process and fail the RDMA. With the predicate, DISCONNECTED replicas are master-side bookkeeping only and never appear in reader-facing lookups.

### 4. Persistent storage identity (`local_disk_segment_id`)

The marker file is the load-bearing identity primitive. `Client::Client()` bootstrap rule:

```
marker_path = ${storage_path}/.mooncake_local_disk_segment_id
data_present = (any data files exist under storage_path)

if marker exists:
    local_disk_segment_id = read(marker)             # normal restart
elif !marker && !data_present:
    local_disk_segment_id = generate_uuid()          # first bootstrap
    write(marker, local_disk_segment_id)
elif !marker && data_present:
    AUTO-MIGRATE (default): generate fresh UUID, write marker, log warning
    FATAL if MC_STORE_REJECT_UNMARKED_DATA=1
```

Properties:

- **Identity is bound to the storage location**, not the process or operator config. Move the SSD to a different host with a fresh Mooncake install → same identity (same UUID reads from the SSD marker). Delete the storage path → new identity on next bootstrap.

**Two IDs, two purposes**:

- `client_id` — random per `Client::Client()` invocation; identifies "the process currently talking to master"; used for liveness, heartbeats, refcount ownership, DRAM segment registration.
- `local_disk_segment_id` — read from marker file on the SSD; identifies "this storage location"; used only for warm re-adoption matching.

A vLLM restart creates a new `client_id` (the new process is a new process — new PID, new RDMA endpoint, new pinned memory) but reads the same `local_disk_segment_id` (the SSD didn't change). The master sees `client_id` change and `local_disk_segment_id` stable → "this is the storage returning under a new process."

### 5. Adoption via extended `MountLocalDiskSegment`

Extend the existing mount:

```cpp
struct MountLocalDiskSegmentRequest {
    UUID client_id;                          // existing
    UUID local_disk_segment_id;               // NEW
    bool enable_offloading;
    uint32_t expected_segment_epoch;          // NEW; 0 = no expectation
};

struct MountLocalDiskSegmentResponse {
    enum class Outcome { FreshMount, ReattachedFromDisconnected };
    Outcome outcome;
    std::vector<std::string> adopted_keys;
    uint32_t new_segment_epoch;
};
```

Master logic: look up `local_disk_segment_id` in the `disconnected_segments_` map. If present and epoch matches (or none expected) → atomic `DISCONNECTED → OK` transition, return `ReattachedFromDisconnected` with the key list. If absent → normal `FreshMount`.

### 6. Race mitigations

Six mechanisms; each is individually necessary for correctness.


| Mechanism                                                              | Code shape                                             | Without it...                                                                            |
| ---------------------------------------------------------------------- | ------------------------------------------------------ | ---------------------------------------------------------------------------------------- |
| `DISCONNECTED` state + grace timer + EvictionThread reaper             | new state machine (§1)                                 | No warm re-adoption                                                                      |
| `local_disk_segment_id` marker file (§4)                               | client bootstrap + master map                          | No way to recognize "A is back"                                                          |
| `fn_is_visible_to_reader` predicate                                    | one new predicate, single call-site change             | Readers RDMA-connect to dead endpoints                                                   |
| Eviction guard: skip keys whose only durable replica is `DISCONNECTED` | predicate in `BatchEvict`                              | Eviction kills DRAM of keys with DISCONNECTED-only LOCAL_DISK → permanent loss           |
| In-flight promotion-task cancellation on OK→DISCONNECTED               | walk `promotion_tasks`, `dec_refcnt()` on local_disk source, drop task | Promotion source local_disk refcnt leaks (`master_service.cpp:2890` `inc_refcnt()` on LOCAL_DISK); replica becomes inert (refcnt > 0 blocks eviction even after reaper sweep). Scope is limited to promotion: offloading and replication `inc_refcnt()` calls (lines 1399, 1935, 2151) land on memory replicas, which `CleanupStaleHandles` + `has_invalid_mem_handle()` already clean up. |
| Seeded `client_ttl` at new-leader boot                                 | three lines before the `ClientMonitorFunc` main loop: pre-populate `client_ttl[id] = now + client_live_ttl_sec` for every distinct `client_id` in `segment_manager_` | `ClientMonitorFunc` populates `client_ttl` only from inbound pings (`master_service.cpp:4923`). On a new leader, dead clients never ping, so they never enter `client_ttl`, so the expiry path never fires for them. Their LOCAL_DISK replicas — restored from snapshot as state=OK because DISCONNECTED is in-memory only — stay visible to readers indefinitely, routing reads to dead RDMA endpoints. Seeding gives each snapshot-restored `client_id` a one-time deadline at `boot + client_live_ttl_sec`; live clients refresh it via heartbeats, dead clients let it expire, and the existing expiry path transitions them to DISCONNECTED. |


**Distinction from existing `grace_period_ms`** (`UnmountSegmentById` at `client_service.h:313`): the codebase already has a per-segment cooperative-shutdown grace (`MasterService::GracefulUnmountSegment` + `GracefulUnmountScheduler`). It is triggered by an explicit client RPC ("I'm going away on purpose, hold the segment for N ms so in-flight readers can finish"), keeps the segment visible during the grace window, and ends in a final commit-unmount. This RFC's `disconnect_grace_period_sec` is a sibling-but-distinct mechanism: triggered by master observation of a TTL trip (unplanned death), hides the segment during the grace window, and can end in re-adoption rather than final removal. Different triggers, opposite visibility semantics, different outcomes — they cannot share a knob.

**Failover semantics:**

- Live clients see no cache blackout. `GetReplicaList` gates only on `fn_is_completed`, so a snapshot-restored OK replica stays visible regardless of `ok_client_` membership. The RDMA data path does not flow through master, so live clients continue serving the moment the new leader can answer RPCs.
- Dead clients are re-detected on the new leader within `client_live_ttl_sec` of boot via seeded `client_ttl`, then hidden by the visibility predicate. Reader behavior during that window is RDMA-fail-then-fallback; after it, clean miss.
- DISCONNECTED state is not persisted across leader failover. Replicas that were DISCONNECTED on the previous leader come back as OK, are re-marked DISCONNECTED by seeded `client_ttl` expiry, and resume their grace window from the new-leader-boot timestamp. Worst-case grace extends to `client_live_ttl_sec + disconnect_grace_period_sec`.

Out of scope:

- **Heartbeat queue lossiness**: pre-existing concern. Unrelated to this RFC.
- **HA durability of DISCONNECTED state**: deferred. The msgpack serialization (positional, via YLT_REFL) plus the strict `kSnapshotSerializerVersion` check make additive schema changes a hard cutover; coordinating with broader rolling-upgrade work (RFC #1920, PR #1776) is the right time to revisit. In-memory + seeded `client_ttl` is correctness-safe in the meantime.
- **Adoption-time data validation** (defense in depth): a future enhancement could spot-check a few file sizes against metadata during adoption to catch on-disk corruption. Low priority; not blocking.

### 7. Configuration

```yaml
master:
  disconnect_grace_period_sec: 600    # NEW (this RFC). OK→DISCONNECTED→REMOVED grace
                                      # window for unplanned client death. Distinct
                                      # from the existing grace_period_ms on
                                      # UnmountSegmentById (cooperative shutdown).
  max_adoption_attempts: 3             # NEW. Cap on DISCONNECTED→OK cycles per segment.
  client_live_ttl_sec: <unchanged>     # Existing knob (default 10 s; 60 s in
                                      # master.json/master.yaml). Not modified by
                                      # this RFC — bumping it would just delay DRAM
                                      # reclaim and isn't needed for the SSD warm
                                      # window (which disconnect_grace_period_sec
                                      # already provides).

client:
  # No persistent-identity config. Identity lives in
  # ${storage_filepath}/.mooncake_local_disk_segment_id, managed by
  # Client::Client() bootstrap. Operators do NOT set local_disk_segment_id
  # directly — it's a data-bound identity, not an operator-managed label.
```

### 8. Metrics

```
mooncake_master_local_disk_replicas{state="ok|disconnected"} gauge
mooncake_master_disconnected_segments_total counter
mooncake_master_adoption_attempts_total{outcome="success|epoch_mismatch|rejected"} counter
mooncake_master_disconnect_grace_expirations_total counter
mooncake_master_disconnect_window_seconds histogram
```

## Implementation Plan

DISCONNECTED state is in-memory only and not serialized. The snapshot format serializes `LocalDiskDescriptor` via YLT_REFL (positional msgpack) and `kSnapshotSerializerVersion` (`catalog_backed_snapshot_provider.cpp:29`) is a hard string match, so adding fields would be a hard cutover. The design follows the existing `GracefulUnmountScheduler` precedent (in-memory, lost on failover); failover-safety comes from the seeded `client_ttl` mechanism in §6.

**Files touched:**

- `replica.h`: add `state` (in-memory enum) + `disconnect_grace_expiry` to `LocalDiskDescriptor`; mark non-serialized.
- `master_service.{h,cpp}`: modify `CleanupStaleHandles` to transition LOCAL_DISK to DISCONNECTED instead of erasing; add `disconnected_segments_` map keyed by `local_disk_segment_id`; add reaper sweep (piggyback on `EvictionThread`); seed `client_ttl` at `ClientMonitorFunc` startup from `segment_manager_`; modify `MountLocalDiskSegment` for adoption; new visibility predicate; eviction guard.
- `client_service.cpp`: marker-file bootstrap (`O_CREAT | O_EXCL` write, normal restart read, moved-disk error, auto-migrate path).
- `master_client.{h,cpp}`, `rpc_types.h`, `rpc_service.{h,cpp}`, `segment.{h,cpp}`: wire new fields on `MountLocalDiskSegmentRequest/Response`.
- Config + metrics: §7 and §8.
- Deployment docs: marker file location, strict-mode env var.

**PR plan** (split for reviewability):

- **PR1 — Core warm re-adoption.** State machine (§1), `LocalDiskDescriptor` extension (§2, in-memory fields), reader visibility predicate (§3), marker file bootstrap (§4), adoption RPC extension (§5), eviction guard, reaper, seeded `client_ttl`. The end-to-end fix for #2254.
- **PR2 — Promotion task cancellation.** Narrow scope: walk `promotion_tasks` on OK→DISCONNECTED, drop the local_disk source refcnt, remove the task. Small; depends on promotion-on-hit landing more fully.
- **Future PR — HA durability of DISCONNECTED state.** Either a snapshot version bump or an etcd sidecar for disconnect records. Wait for broader rolling-upgrade work (RFC #1920) so the schema migration can be coordinated; correctness-safe to defer because PR1 + seeded `client_ttl` already makes failover behavior no-worse-than-today.

**Tests:** `local_disk_disconnect_test.cpp` (one test per race mechanism, including `SeededClientTtlExpiresDeadClientPostFailover`), `local_disk_segment_id_bootstrap_test.cpp` (marker file + moved-disk error), pybind end-to-end. No snapshot round-trip test — disconnected state is intentionally not serialized.

**Acceptance:** issue #2254 reproducer hit rate ≥95% within 1s of `MountLocalDiskSegment`; moved-disk case fails loud; post-failover behavior for dead clients matches today within `client_live_ttl_sec` of new leader boot; no live-client regression on failover; no other regressions.

**Coordination:** RFC #1920 author for future HA-durability follow-up.

## Backward Compatibility

- `GetReplicaList`, `NotifyOffloadSuccess`, `ReMountSegment` unchanged.
- `MountLocalDiskSegment` gains optional request fields and richer response. Old clients omitting them get fresh-mount only (no adoption path).
- `LocalDiskDescriptor`: `state` and `disconnect_grace_expiry` are in-memory only, not serialized; YLT_REFL emission unchanged. No snapshot/oplog wire-format changes.
- `client_live_ttl_sec` is **not** modified by this RFC. The existing default (10 s in code, 60 s in shipped config) is correct for this design.
- Existing `grace_period_ms` on `UnmountSegmentById` is unaffected (different mechanism, different trigger, different semantics — see §6).

## Migration & Rollout

### What an existing deployment looks like at upgrade time

Pre-upgrade state:

- `${storage_path}` has data files (Bucket/.metadata files, FilePerKey objects, or OffsetAllocator data blob).
- No `.mooncake_local_disk_segment_id` marker file (old client code never wrote it).
- Master is running with random `client_id` per client process (today's behavior).

### Bootstrap rule

Restating §Design.4 in the rollout context: the rule must auto-migrate by default for zero-touch upgrade compatibility, with the strict mode (which catches the "I moved my SSD between hosts" footgun) made opt-in:

```
marker present:                            read marker (normal)
marker absent && no data:                  generate, write (fresh bootstrap)
marker absent && data present:             AUTO-MIGRATE — generate fresh UUID,
                                            write marker, log loud warning
marker absent && data present
   && MC_STORE_REJECT_UNMARKED_DATA=1:     FATAL (opt-in strict mode)
```

The auto-migration path logs a `WARNING` like:

```
W mooncake_client.cpp:NNN] Found existing data at /storage but no
.mooncake_local_disk_segment_id marker. Auto-migrating: generated new
id=<uuid> and wrote marker. If this disk wasn't expected to have this
data (e.g., disk moved from another host), STOP THIS SERVICE NOW and
restore the correct disk. Set MC_STORE_REJECT_UNMARKED_DATA=1 to make
this case fatal instead of auto-migrating.
```

Emits metric `mooncake_client_unmarked_data_auto_migrated_total`. Monitoring should alert on this counter going non-zero (it should fire exactly once per host across the upgrade).

### Compatibility matrix


| Master version | Client library version | Behavior                                                                                                                                     |
| -------------- | ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| old            | old                    | today's behavior (the bug)                                                                                                                   |
| **new**        | old                    | old client sends no `local_disk_segment_id` → master treats as no-identity → today's behavior on that client (no benefit, no regression)     |
| old            | **new**                | new client writes marker file but old master ignores `local_disk_segment_id` in mount request → today's behavior (no benefit, no regression) |
| **new**        | **new**                | full warm re-adoption                                                                                                                        |


Mixed versions are safe. The feature kicks in only when both sides are new. Operators can roll out master and clients independently and in any order.

### Recommended rollout sequence

For an HA Mooncake deployment (1 master leader + N standbys + M clients):

1. **Upgrade Mooncake master binary** on standby instances first, then trigger leader failover. Standby instances start fresh (no persistent state); leader's existing in-memory state is dropped on failover (today's behavior). No data risk.
2. **Verify master health** with monitoring (`mooncake_master_local_disk_replicas` should show stable counts).
3. **Upgrade client library** (Mooncake wheel) on one client at a time. Rolling restart:
  - First restart: marker missing + data present → auto-migrate. Warning logged. Marker now exists.
  - Subsequent restarts: marker present → normal path. Warm re-adoption active.
4. **Confirm warm re-adoption is working**: kill -9 one client, restart, verify hit rate recovers within 1s (`mooncake_master_disconnect_window_seconds` histogram should show p99 < 1s for reattached segments).
5. **Optionally enable strict mode** with `MC_STORE_REJECT_UNMARKED_DATA=1` once all hosts have markers written. This protects against future moved-disk footguns.

For non-HA single-master deployments: upgrade master, then roll clients. Same end state.

### vLLM-side changes required: **none**

`MooncakeStoreConnector` calls `mooncake.Client(...)` via Python bindings. The new behavior (marker file read, auto-migrate, etc.) is internalized in `Client::Client()` C++ logic. The Python API surface doesn't change. vLLM operators only need to update the Mooncake wheel pinned in their image / requirements file and restart vLLM workers.

Caveat: if a vLLM worker creates multiple `Client` instances sharing the same `storage_path` (rare but possible if vLLM uses multiprocessing with shared volumes), only the first should bootstrap the marker. The implementation uses `O_CREAT | O_EXCL` file creation to ensure single-writer discipline; secondary callers read the existing marker.

### Rollback safety

- **Marker file is left in place**. Old client code doesn't read it; the file is just a hidden file under storage_path. Re-upgrading later picks it up again.
- **No snapshot or oplog wire-format changes.** Rolling back the master binary doesn't break old masters reading existing snapshots; the in-memory DISCONNECTED state is simply lost on revert (same semantics as today's `GracefulUnmountScheduler` state).

### What operators must NOT do

- **Do not copy marker file between hosts.** It's a host-bound identity. Copying it tricks the master into thinking a fresh client is a returning one and will cause incorrect adoption.
- **Do not delete the marker file while data is present** without considering implications. The default auto-migrate path will silently accept whatever data happens to be present.

These caveats belong in the deployment documentation alongside the marker file's location.

## Alternatives Considered

**A: Keep immediate erasure, rely on ScanMeta.** Today's design. Rejected: 30s–5min gap with cross-instance miss; OffsetAllocator-broken; doesn't address #2254.

**B: Persistent `client_id` only.** Partially adopted as the marker-file identity in §4. Insufficient alone — without delayed deletion, master either keeps dead state forever or erases immediately.

**C: External membership service.** Adds an operational component. Rejected: master is already the membership authority via `ok_client`_.

**D: Per-key TTL instead of per-segment.** Rejected: timer-state explosion, no coherent adoption granularity.

**E: Connector-level fix in vLLM.** Rejected: doesn't help sglang/HiCache/other consumers.

**F: Client Graceful Shutdown + ScanMeta.** Composes, doesn't replace. Crashes still hit the gap.

**G: Deploy with** `replica_num >= 2`**.** The architecturally clean answer for cross-instance availability. Pros: works during downtime, survives permanent loss. Cons: 2× SSD + 2× bandwidth, operator-level config, two divergent copies on rejoin. **Out of scope but recommended for clusters that need cross-instance availability during downtime**. Composes with this RFC for full production-grade configuration.

**H: Use** `DISK` **(3FS / shared FS) instead of** `LOCAL_DISK` **as the offload tier.** `DiskReplicaData` has no `client_id` — file_path is independent of any client process, so client restart doesn't affect master metadata. The bug doesn't exist for DISK-backed deployments. Pros: zero engineering work, full cross-instance availability today, survives any client failure. Cons: read latency goes from ~50µs (local SSD + RDMA) to ~ms (3FS metadata + network read). **Out of scope but the right answer for deployments where shared-FS latency is acceptable.** For hot KV cache, the latency cost typically isn't acceptable; for warm/cold tier, it's fine. Production deployments often use both — LOCAL_DISK hot tier (subject to this bug) + DISK cold tier (not).

## Test Plan

`local_disk_disconnect_test.cpp` (new): one test per mechanism — `DisconnectedReplicaInvisibleToReader`, `EvictionSkipsDisconnectedOnlyKeys`, `SegmentIdAutoReattach`, `MovedDiskMarkerMismatch`, `EpochMismatchRejected`, `MaxAdoptionAttempts`, `PromotionTaskCancelledOnExpiry`, `ConcurrentNotifyDuringAdoption`.

`local_disk_segment_id_bootstrap_test.cpp` (new): marker read/write, fresh bootstrap, normal restart, moved-disk error case.

Extend `pybind_client_test.cpp`: kill -9 + restart + auto-reattach end-to-end.

`high_availability_k8s_test.cpp` gains `SeededClientTtlClosesFailoverLeak`: kill A, trigger master leader failover, assert that A's LOCAL_DISK replicas are hidden from readers within `client_live_ttl_sec` of the new leader's boot (via seeded `client_ttl` expiry, not via HA-durable DISCONNECTED state).

End-to-end reproducer for #2254 in `mooncake-wheel/tests/test_warm_readoption.py`: spin master + two clients, populate SSD, kill A, restart with same `local_disk_segment_id`, assert SSD-hit-rate ≥95% within 1s.

## References

- Issue [#2254](https://github.com/kvcache-ai/Mooncake/issues/2254) — bug
- RFC [#1920](https://github.com/kvcache-ai/Mooncake/issues/1920) — Rolling Upgrade (oplog/snapshot compatibility coordination)
- RFC [#1648](https://github.com/kvcache-ai/Mooncake/issues/1648) — K8s Lease HA (style reference)
- RFC [#1650](https://github.com/kvcache-ai/Mooncake/issues/1650) — HA OpLog Abstraction (oplog reference)
- PR [#2077](https://github.com/kvcache-ai/Mooncake/pull/2077) — Master restart recovery (merged sibling)
- PR [#2215](https://github.com/kvcache-ai/Mooncake/pull/2215) — OffsetAllocator restart recovery (composes)
- PR [#1876](https://github.com/kvcache-ai/Mooncake/pull/1876) — Master HA recovery based on client (composes)
- vLLM PR [#43701](https://github.com/vllm-project/vllm/pull/43701) — DummyClient mode (parallel architectural fix)
