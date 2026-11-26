#pragma once

#include <huxerui/file.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace git_preview {

enum class WorkspacePage { History, Changes, Conflicts };

inline constexpr float kDefaultFilesWidth = 312.0F;

/// A reusable location. A grant-backed location has a display hint and a key in the SDK access store.
struct SavedLocation {
  std::string id;
  std::string path;
  std::string name;
  bool grant = false;
  bool operator==(const SavedLocation&) const = default;
};

/// Navigation intent for one worktree. Review tokens and Git operation data remain live.
struct SavedTab {
  std::string location;
  WorkspacePage page = WorkspacePage::History;
  std::string branch;
  std::string commit;
  std::size_t parent = 0;
  std::string path;
  bool staged = false;
  bool split = false;
  std::string anchor;
  bool operator==(const SavedTab&) const = default;
};

/// Ordered recent locations and tabs, plus the geometry of the desktop review panes.
struct SavedWorkspace {
  std::vector<SavedLocation> locations;
  std::vector<SavedTab> tabs;
  std::string active;
  float history_share = 0.5F;
  float files_width = kDefaultFilesWidth;
  bool operator==(const SavedWorkspace&) const = default;
};

/// Encodes the application-owned, versioned session record.
/// @param snapshot Complete workspace state to publish.
std::string EncodeWorkspace(const SavedWorkspace& snapshot);

/// Validates the version, bounds and location references before exposing the saved state.
/// @param bytes Contents of the application session file.
/// @return The decoded workspace or an empty result for malformed or unsupported data.
std::optional<SavedWorkspace> DecodeWorkspace(std::string_view bytes);

} // namespace git_preview
