#pragma once

#include <string>

#include <ylt/util/tl/expected.hpp>

#include "types.h"

namespace mooncake {

// Filename of the marker file written under the LOCAL_DISK storage path.
// Holds the persistent local_disk_segment_id (UUID, ascii "first-second"
// form) that identifies the storage location across client process
// restarts. See RFC §4.
inline constexpr const char* kLocalDiskMarkerFileName =
    ".mooncake_local_disk_segment_id";

// Environment variable name. When set to "1", an existing storage_path
// that already contains data files but has no marker is treated as a
// fatal misconfiguration (operator likely moved the SSD). Default
// behavior is auto-migration with a loud warning.
inline constexpr const char* kRejectUnmarkedDataEnvVar =
    "MC_STORE_REJECT_UNMARKED_DATA";

// Resolves the persistent local_disk_segment_id for a given storage_path,
// implementing the four-case bootstrap rule (RFC §4):
//
//   marker present                : read marker (normal restart)
//   marker absent && no data      : generate, write (fresh bootstrap)
//   marker absent && data present : auto-migrate; write new marker; log warning
//   marker absent && data present
//     && MC_STORE_REJECT_UNMARKED_DATA=1 : return INVALID_PARAMS (fatal)
//
// The marker is written with O_CREAT | O_EXCL to enforce single-writer
// discipline; concurrent callers fall through to the read path. Returns
// the resolved UUID or an error.
tl::expected<UUID, ErrorCode> ResolveLocalDiskSegmentId(
    const std::string& storage_path);

}  // namespace mooncake
