#include "workspace_session.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_set>

namespace git_preview {
namespace {

constexpr auto kVersion = "lib-git-workspace-v1";
constexpr std::size_t kMaxBytes = 512 * 1024;
constexpr std::size_t kMaxLocations = 128;

bool ValidField(std::string_view value) {
  return value.size() <= 16384 && value.find('\0') == std::string_view::npos;
}

} // namespace

std::string EncodeWorkspace(const SavedWorkspace& snapshot) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << kVersion << '\n' << std::quoted(snapshot.active) << ' '
      << std::setprecision(std::numeric_limits<float>::max_digits10) << snapshot.history_share << ' '
      << snapshot.files_width << '\n' << snapshot.locations.size() << ' ' << snapshot.tabs.size() << '\n';
  for (const auto& location : snapshot.locations) {
    out << std::quoted(location.id) << ' ' << std::quoted(location.path) << ' '
        << std::quoted(location.name) << ' ' << location.grant << '\n';
  }
  for (const auto& tab : snapshot.tabs) {
    out << std::quoted(tab.location) << ' ' << static_cast<int>(tab.page) << ' ' << std::quoted(tab.branch) << ' '
        << std::quoted(tab.commit) << ' ' << tab.parent << ' ' << std::quoted(tab.path) << ' '
        << tab.staged << ' ' << tab.split << ' ' << std::quoted(tab.anchor) << '\n';
  }
  return std::move(out).str();
}

std::optional<SavedWorkspace> DecodeWorkspace(std::string_view bytes) {
  if (bytes.size() > kMaxBytes) return {};
  std::istringstream in{std::string(bytes)};
  in.imbue(std::locale::classic());
  std::string version;
  SavedWorkspace snapshot;
  std::size_t locations = 0;
  std::size_t tabs = 0;
  if (!(in >> version >> std::quoted(snapshot.active) >> snapshot.history_share >> snapshot.files_width
           >> locations >> tabs) || version != kVersion || locations > kMaxLocations || tabs > locations ||
      !std::isfinite(snapshot.history_share) || !std::isfinite(snapshot.files_width)) return {};
  snapshot.history_share = std::clamp(snapshot.history_share, 0.22F, 0.77F);
  snapshot.files_width = std::clamp(snapshot.files_width, 252.0F, 475.0F);

  std::unordered_set<std::string> ids;
  snapshot.locations.resize(locations);
  for (auto& location : snapshot.locations) {
    if (!(in >> std::quoted(location.id) >> std::quoted(location.path) >> std::quoted(location.name) >> location.grant) ||
        location.id.empty() || location.path.empty() || !ValidField(location.id) || !ValidField(location.path) ||
        !ValidField(location.name) || !ids.insert(location.id).second) return {};
  }
  std::unordered_set<std::string> open;
  snapshot.tabs.resize(tabs);
  for (auto& tab : snapshot.tabs) {
    int page = 0;
    if (!(in >> std::quoted(tab.location) >> page >> std::quoted(tab.branch) >> std::quoted(tab.commit) >> tab.parent
             >> std::quoted(tab.path) >> tab.staged >> tab.split >> std::quoted(tab.anchor)) ||
        page < 0 || page > static_cast<int>(WorkspacePage::Conflicts) || !ids.contains(tab.location) ||
        !open.insert(tab.location).second || !ValidField(tab.branch) || !ValidField(tab.commit) ||
        !ValidField(tab.path) || !ValidField(tab.anchor)) return {};
    tab.page = static_cast<WorkspacePage>(page);
  }
  if ((!snapshot.active.empty() && !open.contains(snapshot.active)) || (in >> std::ws && !in.eof())) return {};
  return snapshot;
}

} // namespace git_preview
