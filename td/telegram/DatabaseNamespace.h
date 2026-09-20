// Opaque application cache identity. Checked after ordinary binlog loading,
// before SQLite initialization/destruction and manager/event replay.
// Endpoints and keys are not identity.
#pragma once

#include "td/utils/Slice.h"
#include "td/utils/Status.h"

namespace td {

inline Status check_database_namespace(Slice requested, Slice stored, bool has_existing_data) {
  if (!stored.empty() && requested != stored) {
    return Status::Error(400, "DATABASE_NAMESPACE_MISMATCH");
  }
  if (!requested.empty() && stored.empty() && has_existing_data) {
    return Status::Error(400, "DATABASE_NAMESPACE_REQUIRED");
  }
  return Status::OK();
}

}  // namespace td
