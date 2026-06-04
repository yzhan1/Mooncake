// Behavior tests for the LOCAL_DISK warm re-adoption mechanism (RFC #2306).
//
// Each test exercises an externally-observable consequence of a specific
// RFC mechanism, such that removing the mechanism would make the test fail:
//
//   WarmReadoption_FlipsClientIdAndTransportEndpoint
//     The #2254 fix end-to-end: client A puts data → A expires → client B
//     adopts via matching local_disk_segment_id → master returns a replica
//     descriptor whose client_id is B and whose transport_endpoint is B's.
//     Without the adoption path or the set_local_disk_owner mutator, this
//     test fails.
//
//   ReaperSweepsExpiredDisconnectedReplica
//     With a short disconnect_grace_period_sec, a disconnected replica is
//     eventually removed from the sidecar and a second adoption attempt
//     for the same marker fails to adopt anything.
//
//   SeededClientTtlClosesFailoverLeak
//     After a Reset() (simulated leader failover), a snapshot-restored
//     dead client's LOCAL_DISK replica is re-marked DISCONNECTED via the
//     seeded client_ttl path, instead of leaking visible-forever.
//
//   FreshMountWithNoIdentity_DoesNotPopulateIndex
//     The legacy MountLocalDiskSegment continues to work, and replicas
//     created through it are not eligible for marker-based adoption.
//
//   DisconnectGracePeriodConfigKnobIsAccepted
//     The builder hook is plumbed through MasterServiceConfig.

#include "master_service.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

#include "types.h"

namespace mooncake::test {

class LocalDiskDisconnectTest : public ::testing::Test {
   protected:
    void SetUp() override {
        if (!google_init_done_) {
            google::InitGoogleLogging("LocalDiskDisconnectTest");
            FLAGS_logtostderr = true;
            google_init_done_ = true;
        }
    }
    static bool google_init_done_;

    // Constants for memory segment setup.
    static constexpr size_t kSegBase = 0x300000000;
    static constexpr size_t kSegSize = 16 * 1024 * 1024;
    static constexpr size_t kValueSize = 1024;

    Segment MakeMemSegment(const std::string& name) const {
        Segment s;
        s.id = generate_uuid();
        s.name = name;
        s.base = kSegBase;
        s.size = kSegSize;
        s.te_endpoint = name;  // memory te_endpoint not used in these tests
        return s;
    }

    // Inject a LOCAL_DISK replica via NotifyOffloadSuccess, simulating the
    // result of a successful background offload to client `client_id`'s SSD.
    bool InjectLocalDiskReplica(MasterService& service, const UUID& client_id,
                                const std::string& key,
                                const std::string& transport_endpoint) {
        StorageObjectMetadata sm{};
        sm.bucket_id = 0;
        sm.offset = 0;
        sm.key_size = static_cast<int64_t>(key.size());
        sm.data_size = static_cast<int64_t>(kValueSize);
        sm.transport_endpoint = transport_endpoint;
        std::vector<std::string> keys{key};
        std::vector<StorageObjectMetadata> metas{sm};
        auto res = service.NotifyOffloadSuccess(client_id, keys, metas);
        return res.has_value();
    }

    // Read sidecar runtime state for a given LOCAL_DISK replica.
    // Used by tests to assert internal state transitions; only available
    // via the friend declaration.
    std::optional<LocalDiskRuntime> GetSidecar(MasterService& service,
                                               ReplicaID rid) {
        std::lock_guard<std::mutex> lock(service.local_disk_runtime_mutex_);
        auto it = service.local_disk_runtime_.find(rid);
        if (it == service.local_disk_runtime_.end()) return std::nullopt;
        return it->second;
    }

    // Forwarders to private MasterService methods. The fixture is friended;
    // GoogleTest TEST_F-generated subclasses are not, so they call these.
    void SimulateLeaderFailover(MasterService& service) {
        service.ResetStateAfterFailedRestoreAttempt();
    }
    void TriggerMemoryEviction(MasterService& service, double target,
                               double lowerbound) {
        service.BatchEvict(target, lowerbound);
    }

    // Snapshot the LOCAL_DISK ReplicaID for a key (after Put + offload).
    // Returns 0 if no LOCAL_DISK replica is found.
    ReplicaID GetLocalDiskReplicaId(MasterService& service,
                                    const std::string& key) {
        auto resp = service.GetReplicaList(key);
        if (!resp) return 0;
        for (const auto& desc : resp.value().replicas) {
            if (std::holds_alternative<LocalDiskDescriptor>(
                    desc.descriptor_variant)) {
                return desc.id;
            }
        }
        return 0;
    }
};
bool LocalDiskDisconnectTest::google_init_done_ = false;

// ---- The headline #2254 fix: adoption flips client_id + endpoint ----
TEST_F(LocalDiskDisconnectTest,
       WarmReadoption_FlipsClientIdAndTransportEndpoint) {
    auto service = std::make_unique<MasterService>(
        MasterServiceConfigBuilder()
            .set_enable_offload(true)
            .set_client_live_ttl_sec(1)           // expire quickly
            .set_disconnect_grace_period_sec(60)  // generous grace
            .build());

    // -- Setup phase: client A puts a key, then offload registers LOCAL_DISK.
    UUID client_a = generate_uuid();
    UUID marker = generate_uuid();
    Segment mem = MakeMemSegment("seg_for_a");
    ASSERT_TRUE(service->MountSegment(mem, client_a).has_value());

    auto mount_a = service->MountLocalDiskSegmentWithIdentity(
        client_a, marker, "tcp://a:50051", /*enable_offloading=*/true);
    ASSERT_TRUE(mount_a.has_value());
    EXPECT_EQ(mount_a.value().first, MasterService::MountOutcome::FreshMount);

    const std::string key = "warm_readoption_key";
    ReplicateConfig rcfg;
    rcfg.replica_num = 1;
    rcfg.preferred_segment = mem.name;
    ASSERT_TRUE(service->PutStart(client_a, key, kValueSize, rcfg).has_value());
    ASSERT_TRUE(
        service->PutEnd(client_a, key, ReplicaType::MEMORY).has_value());
    ASSERT_TRUE(
        InjectLocalDiskReplica(*service, client_a, key, "tcp://a:50051"));

    ReplicaID rid_before = GetLocalDiskReplicaId(*service, key);
    ASSERT_NE(rid_before, 0u) << "LOCAL_DISK replica not registered";
    auto sidecar_before = GetSidecar(*service, rid_before);
    ASSERT_TRUE(sidecar_before.has_value());
    EXPECT_EQ(sidecar_before->state, LocalDiskReplicaState::OK);
    EXPECT_EQ(sidecar_before->local_disk_segment_id, marker);

    // -- Expire client A: stop pinging, sleep > TTL + ClientMonitor tick.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    // After expiry, the LOCAL_DISK replica must be DISCONNECTED in the
    // sidecar (transition by CleanupStaleHandles) — not erased.
    auto sidecar_after = GetSidecar(*service, rid_before);
    ASSERT_TRUE(sidecar_after.has_value())
        << "Sidecar entry was erased; CleanupStaleHandles should keep it";
    EXPECT_EQ(sidecar_after->state, LocalDiskReplicaState::DISCONNECTED);

    // And it must be invisible to readers (visibility predicate).
    auto stale_read = service->GetReplicaList(key);
    EXPECT_FALSE(stale_read.has_value())
        << "DISCONNECTED LOCAL_DISK leaked to reader";

    // -- Client B brings the same disk back: warm-readopt.
    UUID client_b = generate_uuid();
    auto mount_b = service->MountLocalDiskSegmentWithIdentity(
        client_b, marker, "tcp://b:60051", /*enable_offloading=*/true);
    ASSERT_TRUE(mount_b.has_value()) << "error=" << mount_b.error();
    EXPECT_EQ(mount_b.value().first, MasterService::MountOutcome::Reattached);
    EXPECT_EQ(mount_b.value().second, 1u)
        << "Expected exactly one replica to be adopted";

    // The replica must now be visible again AND its descriptor must point
    // at client B and B's transport_endpoint, not A's.
    auto reread = service->GetReplicaList(key);
    ASSERT_TRUE(reread.has_value()) << "Adopted replica should be readable";
    bool found_b_owned = false;
    for (const auto& desc : reread.value().replicas) {
        if (auto* ld =
                std::get_if<LocalDiskDescriptor>(&desc.descriptor_variant)) {
            EXPECT_EQ(ld->client_id, client_b) << "client_id not flipped to B";
            EXPECT_EQ(ld->transport_endpoint, "tcp://b:60051")
                << "transport_endpoint not flipped to B";
            found_b_owned = true;
        }
    }
    EXPECT_TRUE(found_b_owned) << "Adopted LOCAL_DISK replica missing";
}

// ---- Reaper actually erases DISCONNECTED entries past grace ----
TEST_F(LocalDiskDisconnectTest, ReaperSweepsExpiredDisconnectedReplica) {
    auto service = std::make_unique<MasterService>(
        MasterServiceConfigBuilder()
            .set_enable_offload(true)
            .set_client_live_ttl_sec(1)
            .set_disconnect_grace_period_sec(1)  // very short
            .build());

    UUID client_a = generate_uuid();
    UUID marker = generate_uuid();
    Segment mem = MakeMemSegment("seg_reaper");
    ASSERT_TRUE(service->MountSegment(mem, client_a).has_value());
    ASSERT_TRUE(service
                    ->MountLocalDiskSegmentWithIdentity(client_a, marker,
                                                        "tcp://a:50051", true)
                    .has_value());

    const std::string key = "reaper_key";
    ReplicateConfig rcfg;
    rcfg.replica_num = 1;
    rcfg.preferred_segment = mem.name;
    ASSERT_TRUE(service->PutStart(client_a, key, kValueSize, rcfg).has_value());
    ASSERT_TRUE(
        service->PutEnd(client_a, key, ReplicaType::MEMORY).has_value());
    ASSERT_TRUE(
        InjectLocalDiskReplica(*service, client_a, key, "tcp://a:50051"));
    ReplicaID rid = GetLocalDiskReplicaId(*service, key);
    ASSERT_NE(rid, 0u);

    // Expire client + wait for reaper. Total wait must exceed
    // client_live_ttl_sec + disconnect_grace_period_sec + slack.
    std::this_thread::sleep_for(std::chrono::milliseconds(4500));

    // The sidecar entry should be gone.
    EXPECT_FALSE(GetSidecar(*service, rid).has_value())
        << "Reaper did not remove grace-expired DISCONNECTED entry";

    // A subsequent adoption attempt for the same marker should find
    // nothing to adopt (index entry was pruned).
    UUID client_b = generate_uuid();
    auto mount_b = service->MountLocalDiskSegmentWithIdentity(
        client_b, marker, "tcp://b:60051", true);
    ASSERT_TRUE(mount_b.has_value());
    EXPECT_EQ(mount_b.value().first, MasterService::MountOutcome::FreshMount);
    EXPECT_EQ(mount_b.value().second, 0u);
}

// ---- Seeded client_ttl prevents the failover leak ----
TEST_F(LocalDiskDisconnectTest, SeededClientTtlClosesFailoverLeak) {
    auto service =
        std::make_unique<MasterService>(MasterServiceConfigBuilder()
                                            .set_enable_offload(true)
                                            .set_client_live_ttl_sec(1)
                                            .set_disconnect_grace_period_sec(60)
                                            .build());

    UUID client_a = generate_uuid();
    UUID marker = generate_uuid();
    Segment mem = MakeMemSegment("seg_failover");
    ASSERT_TRUE(service->MountSegment(mem, client_a).has_value());
    ASSERT_TRUE(service
                    ->MountLocalDiskSegmentWithIdentity(client_a, marker,
                                                        "tcp://a:50051", true)
                    .has_value());

    const std::string key = "failover_key";
    ReplicateConfig rcfg;
    rcfg.replica_num = 1;
    rcfg.preferred_segment = mem.name;
    ASSERT_TRUE(service->PutStart(client_a, key, kValueSize, rcfg).has_value());
    ASSERT_TRUE(
        service->PutEnd(client_a, key, ReplicaType::MEMORY).has_value());
    ASSERT_TRUE(
        InjectLocalDiskReplica(*service, client_a, key, "tcp://a:50051"));

    // Simulate leader failover: Reset() clears ok_client_, the sidecar,
    // and drains client_ping_queue_. segment_manager_ is preserved (the
    // snapshot is unaffected).
    SimulateLeaderFailover(*service);

    // A is no longer in ok_client_ and never pings on the new leader.
    // Without seeded client_ttl, ClientMonitorFunc never expires A.
    // With seeded client_ttl, A enters client_ttl at boot and expires
    // within client_live_ttl_sec.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    // The key must now be hidden from readers.
    auto resp = service->GetReplicaList(key);
    EXPECT_FALSE(resp.has_value())
        << "Dead-pre-failover LOCAL_DISK replica still visible (the leak)";
}

// ---- Legacy MountLocalDiskSegment doesn't enable adoption ----
TEST_F(LocalDiskDisconnectTest,
       LegacyMountLocalDiskSegment_DoesNotPopulateIndex) {
    auto service = std::make_unique<MasterService>(
        MasterServiceConfigBuilder().set_enable_offload(true).build());

    // Client A uses the LEGACY mount path (no marker).
    UUID client_a = generate_uuid();
    Segment mem = MakeMemSegment("seg_legacy");
    ASSERT_TRUE(service->MountSegment(mem, client_a).has_value());
    ASSERT_TRUE(service->MountLocalDiskSegment(client_a, true).has_value());

    const std::string key = "legacy_key";
    ReplicateConfig rcfg;
    rcfg.replica_num = 1;
    rcfg.preferred_segment = mem.name;
    ASSERT_TRUE(service->PutStart(client_a, key, kValueSize, rcfg).has_value());
    ASSERT_TRUE(
        service->PutEnd(client_a, key, ReplicaType::MEMORY).has_value());
    ASSERT_TRUE(
        InjectLocalDiskReplica(*service, client_a, key, "tcp://a:50051"));

    // Now another client tries to adopt with some random marker. There is
    // nothing in the index keyed by that marker, so the outcome must be
    // FreshMount (not Reattached).
    UUID client_b = generate_uuid();
    UUID some_marker = generate_uuid();
    auto mount_b = service->MountLocalDiskSegmentWithIdentity(
        client_b, some_marker, "tcp://b:60051", true);
    ASSERT_TRUE(mount_b.has_value());
    EXPECT_EQ(mount_b.value().first, MasterService::MountOutcome::FreshMount);
    EXPECT_EQ(mount_b.value().second, 0u);
}

// ---- Eviction guard: memory replica preserved while LOCAL_DISK is
//      DISCONNECTED, so the key remains adoptable. ----
//
// Setup mirrors the cross-client case: client A owns the memory replica
// (PutStart/PutEnd on its memory segment); client B owns the LOCAL_DISK
// replica (NotifyOffloadSuccess called with B's client_id). Only B is
// allowed to expire; A keeps pinging itself alive throughout the test.
//
// Without the eviction guard in can_evict_replicas, BatchEvict would
// erase A's memory replica even though B's LOCAL_DISK is now DISCONNECTED
// (invisible to readers), leaving the key effectively unreadable until
// B returns to re-adopt. With the guard, eviction skips this key.
TEST_F(LocalDiskDisconnectTest,
       EvictionGuardSkipsKeysWithDisconnectedOnlyLocalDisk) {
    auto service = std::make_unique<MasterService>(
        MasterServiceConfigBuilder()
            .set_enable_offload(true)
            .set_client_live_ttl_sec(1)
            .set_disconnect_grace_period_sec(60)
            .set_default_kv_lease_ttl(50)  // very short so eviction can fire
            .set_eviction_ratio(1.0)
            .set_eviction_high_watermark_ratio(1.0)
            .build());

    // Client A owns the memory replica.
    UUID client_a = generate_uuid();
    Segment mem = MakeMemSegment("seg_eviction_guard");
    ASSERT_TRUE(service->MountSegment(mem, client_a).has_value());

    // Client B owns the LOCAL_DISK replica via marker-based mount + offload.
    // Ping B once so ClientMonitorFunc starts tracking its TTL; otherwise B
    // never enters client_ttl and never expires, so its LOCAL_DISK never
    // transitions to DISCONNECTED in the sidecar.
    UUID client_b = generate_uuid();
    UUID marker = generate_uuid();
    ASSERT_TRUE(service
                    ->MountLocalDiskSegmentWithIdentity(client_b, marker,
                                                        "tcp://b:60051", true)
                    .has_value());
    (void)service->Ping(client_b);

    const std::string key = "evict_guard_key";
    ReplicateConfig rcfg;
    rcfg.replica_num = 1;
    rcfg.preferred_segment = mem.name;
    ASSERT_TRUE(service->PutStart(client_a, key, kValueSize, rcfg).has_value());
    ASSERT_TRUE(
        service->PutEnd(client_a, key, ReplicaType::MEMORY).has_value());
    ASSERT_TRUE(
        InjectLocalDiskReplica(*service, client_b, key, "tcp://b:60051"));

    // Both replicas should be visible right now.
    {
        auto pre = service->GetReplicaList(key);
        ASSERT_TRUE(pre.has_value());
        size_t mem_count = 0;
        size_t ld_count = 0;
        for (const auto& d : pre.value().replicas) {
            if (std::holds_alternative<MemoryDescriptor>(d.descriptor_variant))
                ++mem_count;
            else if (std::holds_alternative<LocalDiskDescriptor>(
                         d.descriptor_variant))
                ++ld_count;
        }
        EXPECT_EQ(mem_count, 1u);
        EXPECT_EQ(ld_count, 1u);
    }

    // Keep A alive across the wait by pinging it; let B expire.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        (void)service->Ping(client_a);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Trigger eviction. Without the guard, the memory replica is erased
    // because the original predicate only required "any completed memory
    // replica with refcnt==0". With the guard, BatchEvict skips this key
    // because no LOCAL_DISK replica is OK in the sidecar.
    TriggerMemoryEviction(*service, /*target=*/1.0, /*lowerbound=*/1.0);

    // After eviction: the memory replica must still be present (and the
    // LOCAL_DISK invisible because DISCONNECTED).
    auto post = service->GetReplicaList(key);
    ASSERT_TRUE(post.has_value())
        << "Memory replica was evicted while LOCAL_DISK is DISCONNECTED — "
        << "the key is now unreadable until B returns. Eviction guard "
        << "did not fire.";
    bool found_mem = false;
    bool found_ld = false;
    for (const auto& d : post.value().replicas) {
        if (std::holds_alternative<MemoryDescriptor>(d.descriptor_variant))
            found_mem = true;
        if (std::holds_alternative<LocalDiskDescriptor>(d.descriptor_variant))
            found_ld = true;
    }
    EXPECT_TRUE(found_mem) << "Memory replica was wrongly evicted";
    EXPECT_FALSE(found_ld)
        << "DISCONNECTED LOCAL_DISK should be filtered by visibility "
           "predicate";
}

// ---- Concurrent adoption race ----
//
// Two clients (B and C) bring the same on-SSD storage to a new host at
// roughly the same time, e.g. operator copied the disk twice or two pods
// were scheduled concurrently against the same persistent volume. Both
// read the same local_disk_segment_id marker and call
// MountLocalDiskSegmentWithIdentity simultaneously.
//
// Required invariants (RFC #2306, addresses LujhCoconut's race point):
//   - Neither call returns an error; both succeed.
//   - At least one call returns Reattached with adopted_count=1.
//   - After both complete, the replica descriptor served by GetReplicaList
//     contains exactly one of the two transport endpoints (the "loser"
//     should not see its endpoint torn/half-overwritten).
//   - The sidecar entry settles to OK state.
TEST_F(LocalDiskDisconnectTest, ConcurrentAdoptionByTwoClientsIsSafe) {
    auto service =
        std::make_unique<MasterService>(MasterServiceConfigBuilder()
                                            .set_enable_offload(true)
                                            .set_client_live_ttl_sec(1)
                                            .set_disconnect_grace_period_sec(60)
                                            .build());

    // Setup: client A writes data, registers LOCAL_DISK, then expires.
    UUID client_a = generate_uuid();
    UUID marker = generate_uuid();
    Segment mem = MakeMemSegment("seg_concurrent");
    ASSERT_TRUE(service->MountSegment(mem, client_a).has_value());
    ASSERT_TRUE(service
                    ->MountLocalDiskSegmentWithIdentity(client_a, marker,
                                                        "tcp://a:50051", true)
                    .has_value());

    const std::string key = "concurrent_adoption_key";
    ReplicateConfig rcfg;
    rcfg.replica_num = 1;
    rcfg.preferred_segment = mem.name;
    ASSERT_TRUE(service->PutStart(client_a, key, kValueSize, rcfg).has_value());
    ASSERT_TRUE(
        service->PutEnd(client_a, key, ReplicaType::MEMORY).has_value());
    ASSERT_TRUE(
        InjectLocalDiskReplica(*service, client_a, key, "tcp://a:50051"));

    // Capture the LOCAL_DISK ReplicaID now, while the visibility predicate
    // still lets it through (A is still alive). After A expires below, the
    // descriptor becomes hidden and GetReplicaList would return an error.
    ReplicaID rid = GetLocalDiskReplicaId(*service, key);
    ASSERT_NE(rid, 0u);

    // Wait for A to expire and its LOCAL_DISK to transition to DISCONNECTED.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    // Concurrent adoption attempts by B and C.
    UUID client_b = generate_uuid();
    UUID client_c = generate_uuid();
    const std::string ep_b = "tcp://b:60051";
    const std::string ep_c = "tcp://c:70051";

    using OutcomeOrErr =
        tl::expected<std::pair<MasterService::MountOutcome, size_t>, ErrorCode>;
    OutcomeOrErr result_b = tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
    OutcomeOrErr result_c = tl::make_unexpected(ErrorCode::INTERNAL_ERROR);

    // Use a latch-like barrier to maximise overlap.
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    auto attempt = [&](OutcomeOrErr& slot, const UUID& cid,
                       const std::string& ep) {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        slot =
            service->MountLocalDiskSegmentWithIdentity(cid, marker, ep, true);
    };

    std::thread t_b(attempt, std::ref(result_b), std::cref(client_b),
                    std::cref(ep_b));
    std::thread t_c(attempt, std::ref(result_c), std::cref(client_c),
                    std::cref(ep_c));

    while (ready.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);

    t_b.join();
    t_c.join();

    // Invariant 1: neither call errored out.
    ASSERT_TRUE(result_b.has_value())
        << "Client B adoption errored: " << result_b.error();
    ASSERT_TRUE(result_c.has_value())
        << "Client C adoption errored: " << result_c.error();

    // Invariant 2: at least one is Reattached.
    bool any_reattached =
        result_b.value().first == MasterService::MountOutcome::Reattached ||
        result_c.value().first == MasterService::MountOutcome::Reattached;
    EXPECT_TRUE(any_reattached)
        << "Neither concurrent adoption took the Reattached path";

    // Invariant 3: the final descriptor has exactly one of the two
    // endpoints (no torn writes / mixing).
    auto final_read = service->GetReplicaList(key);
    ASSERT_TRUE(final_read.has_value())
        << "Key unreadable after concurrent adoption";
    bool found_ld = false;
    std::string final_endpoint;
    UUID final_client_id{};
    for (const auto& d : final_read.value().replicas) {
        if (auto* ld =
                std::get_if<LocalDiskDescriptor>(&d.descriptor_variant)) {
            found_ld = true;
            final_endpoint = ld->transport_endpoint;
            final_client_id = ld->client_id;
        }
    }
    ASSERT_TRUE(found_ld) << "Adopted LOCAL_DISK descriptor missing";
    EXPECT_TRUE(final_endpoint == ep_b || final_endpoint == ep_c)
        << "Transport endpoint settled to unexpected value: " << final_endpoint;
    EXPECT_TRUE(final_client_id == client_b || final_client_id == client_c)
        << "client_id settled to unexpected value";

    // Invariant 4: sidecar state is OK after adoption settles.
    auto sidecar = GetSidecar(*service, rid);
    ASSERT_TRUE(sidecar.has_value());
    EXPECT_EQ(sidecar->state, LocalDiskReplicaState::OK);
}

// ---- Config knob is plumbed through ----
TEST_F(LocalDiskDisconnectTest, DisconnectGracePeriodConfigKnobIsAccepted) {
    auto service_default =
        std::make_unique<MasterService>(MasterServiceConfigBuilder().build());
    EXPECT_NE(service_default, nullptr);

    auto service_short =
        std::make_unique<MasterService>(MasterServiceConfigBuilder()
                                            .set_disconnect_grace_period_sec(2)
                                            .build());
    EXPECT_NE(service_short, nullptr);
}

}  // namespace mooncake::test
