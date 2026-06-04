// Tests for the marker-file bootstrap of local_disk_segment_id (RFC §4).
//
// Verifies the four-case rule from ResolveLocalDiskSegmentId:
//   1. marker present                          → read marker (normal restart)
//   2. marker absent && no data                → generate + write (fresh)
//   3. marker absent && data present           → auto-migrate (warn)
//   4. marker absent && data present
//      && MC_STORE_REJECT_UNMARKED_DATA=1      → return INVALID_PARAMS

#include "local_disk_marker.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "types.h"

namespace mooncake::test {

namespace fs = std::filesystem;

class LocalDiskMarkerTest : public ::testing::Test {
   protected:
    void SetUp() override {
        // Each test gets its own scratch dir under the system temp.
        scratch_ = fs::temp_directory_path() /
                   ("mooncake_marker_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(++counter_));
        fs::create_directories(scratch_);
        ::unsetenv(kRejectUnmarkedDataEnvVar);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(scratch_, ec);
        ::unsetenv(kRejectUnmarkedDataEnvVar);
    }
    fs::path scratch_;
    static int counter_;
};
int LocalDiskMarkerTest::counter_ = 0;

// Case 2: empty storage_path → generates a fresh UUID and writes the marker.
TEST_F(LocalDiskMarkerTest, FreshBootstrapEmptyDirGeneratesAndPersists) {
    auto r1 = ResolveLocalDiskSegmentId(scratch_.string());
    ASSERT_TRUE(r1.has_value());
    UUID first = r1.value();
    EXPECT_TRUE(first.first != 0 || first.second != 0);

    const fs::path marker = scratch_ / kLocalDiskMarkerFileName;
    EXPECT_TRUE(fs::exists(marker));

    // Case 1: a second call reads the same UUID back.
    auto r2 = ResolveLocalDiskSegmentId(scratch_.string());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2.value(), first);
}

// Case 3: data present, no marker, default behavior is auto-migrate.
TEST_F(LocalDiskMarkerTest, AutoMigrateWhenDataPresentNoMarker) {
    // Drop a non-marker file in the scratch dir to simulate pre-existing data.
    {
        std::ofstream f(scratch_ / "some_data.bin");
        f << "x";
    }

    auto r = ResolveLocalDiskSegmentId(scratch_.string());
    ASSERT_TRUE(r.has_value())
        << "auto-migrate should succeed by default, got error=" << r.error();
    EXPECT_TRUE(fs::exists(scratch_ / kLocalDiskMarkerFileName));
}

// Case 4: data present, no marker, strict mode → INVALID_PARAMS.
TEST_F(LocalDiskMarkerTest, StrictModeRefusesAutoMigrate) {
    {
        std::ofstream f(scratch_ / "some_data.bin");
        f << "x";
    }
    ::setenv(kRejectUnmarkedDataEnvVar, "1", /*overwrite=*/1);

    auto r = ResolveLocalDiskSegmentId(scratch_.string());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ErrorCode::INVALID_PARAMS);

    // No marker should have been written.
    EXPECT_FALSE(fs::exists(scratch_ / kLocalDiskMarkerFileName));
}

// Case 1 (corrupted marker): existing marker with garbage content fails to
// parse, returning INVALID_PARAMS rather than silently re-generating.
TEST_F(LocalDiskMarkerTest, CorruptedMarkerIsRejected) {
    {
        std::ofstream f(scratch_ / kLocalDiskMarkerFileName);
        f << "not-a-uuid";
    }
    auto r = ResolveLocalDiskSegmentId(scratch_.string());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ErrorCode::INVALID_PARAMS);
}

// Non-existent storage_path → INVALID_PARAMS (caller misconfiguration).
TEST_F(LocalDiskMarkerTest, MissingStoragePathFails) {
    fs::path missing = scratch_ / "does_not_exist";
    auto r = ResolveLocalDiskSegmentId(missing.string());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ErrorCode::INVALID_PARAMS);
}

}  // namespace mooncake::test
