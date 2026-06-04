#include "local_disk_marker.h"

#include <fcntl.h>
#include <glog/logging.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace mooncake {

namespace {

// Returns true if storage_path contains anything that looks like cache data
// already (any regular file or non-empty subdirectory other than the marker
// file itself). Used to drive the auto-migrate vs. fresh-bootstrap decision.
bool HasExistingData(const std::filesystem::path& storage_path) {
    std::error_code ec;
    if (!std::filesystem::exists(storage_path, ec) || ec) {
        return false;
    }
    if (!std::filesystem::is_directory(storage_path, ec) || ec) {
        return false;
    }
    for (std::filesystem::directory_iterator it(storage_path, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (ec) break;
        const auto name = it->path().filename().string();
        if (name == kLocalDiskMarkerFileName) continue;
        if (name == "." || name == "..") continue;
        return true;
    }
    return false;
}

// Reads the marker file content (ASCII UUID in "first-second" form) and
// parses it. Returns the parsed UUID or an error.
tl::expected<UUID, ErrorCode> ReadMarkerFile(
    const std::filesystem::path& marker_path) {
    std::ifstream in(marker_path);
    if (!in) {
        LOG(ERROR) << "marker_path=" << marker_path
                   << ", error=marker_open_failed, errno=" << errno << " ("
                   << std::strerror(errno) << ")";
        return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
    }
    std::string contents;
    std::getline(in, contents);
    UUID uuid;
    if (!StringToUuid(contents, uuid)) {
        LOG(ERROR) << "marker_path=" << marker_path
                   << ", error=marker_parse_failed, contents='" << contents
                   << "'";
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }
    return uuid;
}

// Atomically writes the marker file at marker_path with the ASCII form of
// the given UUID. Uses O_CREAT|O_EXCL so concurrent callers fail rather
// than racing on partial writes. EEXIST is propagated to the caller so
// they can fall back to reading the existing marker.
tl::expected<void, ErrorCode> WriteMarkerFileExclusive(
    const std::filesystem::path& marker_path, const UUID& uuid) {
    const std::string ascii = UuidToString(uuid) + "\n";
    int fd = ::open(marker_path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            return tl::make_unexpected(ErrorCode::OBJECT_ALREADY_EXISTS);
        }
        LOG(ERROR) << "marker_path=" << marker_path
                   << ", error=marker_create_failed, errno=" << errno << " ("
                   << std::strerror(errno) << ")";
        return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
    }
    ssize_t written = 0;
    while (written < static_cast<ssize_t>(ascii.size())) {
        ssize_t n = ::write(fd, ascii.data() + written, ascii.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG(ERROR) << "marker_path=" << marker_path
                       << ", error=marker_write_failed, errno=" << errno << " ("
                       << std::strerror(errno) << ")";
            ::close(fd);
            ::unlink(marker_path.c_str());
            return tl::make_unexpected(ErrorCode::INTERNAL_ERROR);
        }
        written += n;
    }
    if (::fsync(fd) != 0) {
        LOG(WARNING) << "marker_path=" << marker_path
                     << ", warn=marker_fsync_failed, errno=" << errno;
    }
    ::close(fd);
    return {};
}

bool StrictModeEnabled() {
    const char* v = std::getenv(kRejectUnmarkedDataEnvVar);
    return v != nullptr && std::string(v) == "1";
}

}  // namespace

tl::expected<UUID, ErrorCode> ResolveLocalDiskSegmentId(
    const std::string& storage_path) {
    namespace fs = std::filesystem;
    fs::path root(storage_path);
    std::error_code ec;
    if (!fs::exists(root, ec) || ec) {
        LOG(ERROR) << "storage_path=" << storage_path
                   << ", error=storage_path_not_found";
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }
    if (!fs::is_directory(root, ec) || ec) {
        LOG(ERROR) << "storage_path=" << storage_path
                   << ", error=storage_path_not_directory";
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }

    const fs::path marker_path = root / kLocalDiskMarkerFileName;

    // Case 1: marker present → read it.
    if (fs::exists(marker_path, ec) && !ec) {
        return ReadMarkerFile(marker_path);
    }

    const bool data_present = HasExistingData(root);

    // Case 4 (strict mode): marker absent + data present + opt-in fatal.
    if (data_present && StrictModeEnabled()) {
        LOG(ERROR) << "storage_path=" << storage_path
                   << ", error=unmarked_data_present_strict_mode"
                   << ", action=refusing_to_bootstrap"
                   << " (" << kRejectUnmarkedDataEnvVar << "=1, "
                   << kLocalDiskMarkerFileName << " is missing). "
                   << "If you intentionally moved this storage to a new host,"
                   << " unset " << kRejectUnmarkedDataEnvVar
                   << " to allow auto-migration.";
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }

    // Case 2 (fresh bootstrap) and Case 3 (auto-migrate) share write path.
    const UUID fresh_id = generate_uuid();
    auto write_result = WriteMarkerFileExclusive(marker_path, fresh_id);
    if (!write_result) {
        if (write_result.error() == ErrorCode::OBJECT_ALREADY_EXISTS) {
            // Another caller bootstrapped concurrently; read what they wrote.
            return ReadMarkerFile(marker_path);
        }
        return tl::make_unexpected(write_result.error());
    }

    if (data_present) {
        LOG(WARNING)
            << "storage_path=" << storage_path
            << ", action=auto_migrate_unmarked_data"
            << ", new_local_disk_segment_id=" << UuidToString(fresh_id)
            << ". Found existing data but no " << kLocalDiskMarkerFileName
            << " marker. Auto-migrating: "
            << "generated a fresh UUID and wrote the marker. If this disk "
            << "wasn't expected to have this data (e.g., disk moved from "
            << "another host), STOP THIS SERVICE NOW and restore the "
            << "correct disk. Set " << kRejectUnmarkedDataEnvVar
            << "=1 to make this case fatal instead of auto-migrating.";
    } else {
        LOG(INFO) << "storage_path=" << storage_path
                  << ", action=fresh_bootstrap_marker"
                  << ", local_disk_segment_id=" << UuidToString(fresh_id);
    }
    return fresh_id;
}

}  // namespace mooncake
