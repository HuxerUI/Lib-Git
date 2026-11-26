#include <huxerui/huxerui.h>
#include <huxerui/git.h>

#include "workspace_session.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <unordered_map>
#include <variant>

using namespace huxerui;
namespace git = huxerui::git;

namespace {

using Jobs = std::variant<git::Transfer<git::CloneOutcome>, git::Transfer<git::FetchOutcome>,
                          git::Transfer<git::PushOutcome>, git::Transfer<git::PullOutcome>,
                          git::Transfer<git::ChangeOutcome>>;

using git_preview::SavedLocation;
using git_preview::SavedTab;
using git_preview::SavedWorkspace;
using git_preview::WorkspacePage;
enum class RepositoryAction { Open, Clone, Initialize };

std::string DisplayLine(std::string_view text) {
  if (text.ends_with('\n')) text.remove_suffix(1);
  if (text.ends_with('\r')) text.remove_suffix(1);
  if (text.find('\t') == std::string_view::npos) return std::string(text);
  std::string result;
  for (char character : text) {
    if (character == '\t') result.append(4 - result.size() % 4, ' ');
    else result.push_back(character);
  }
  return result;
}

struct DiffRow {
  std::size_t hunk;
  const git::DiffLine* left = nullptr;
  const git::DiffLine* right = nullptr;
};

/// Rows borrow line records from the immutable patch. Virtual list factories retain this owner until they unmount.
struct FileReview {
  explicit FileReview(git::FileDiff value) : diff(std::move(value)) {
    for (std::size_t hunk = 0; hunk < diff.hunks.size(); ++hunk) {
      unified.push_back({hunk});
      split.push_back({hunk});
      const auto& lines = diff.hunks[hunk].lines;
      for (std::size_t index = 0; index < lines.size();) {
        const auto& line = lines[index++];
        const auto append = [&](const git::DiffLine& item) {
          unified.push_back({hunk, &item});
          width = std::max(width, DisplayLine(item.text).size());
        };
        append(line);
        if (line.kind != git::LineKind::Deleted && line.kind != git::LineKind::Added) {
          split.push_back({hunk, &line, line.kind == git::LineKind::Context ? &line : nullptr});
          continue;
        }
        std::vector<const git::DiffLine*> removed;
        std::vector<const git::DiffLine*> added;
        const auto collect = [&](const git::DiffLine& item) {
          (item.kind == git::LineKind::Deleted ? removed : added).push_back(&item);
        };
        collect(line);
        while (index < lines.size() &&
               (lines[index].kind == git::LineKind::Deleted || lines[index].kind == git::LineKind::Added)) {
          append(lines[index]);
          collect(lines[index++]);
        }
        for (std::size_t row = 0; row < std::max(removed.size(), added.size()); ++row) {
          split.push_back({hunk, row < removed.size() ? removed[row] : nullptr, row < added.size() ? added[row] : nullptr});
        }
      }
    }
  }

  FileReview(const FileReview&) = delete;

  FileReview& operator=(const FileReview&) = delete;

  git::FileDiff diff;
  std::vector<DiffRow> unified;
  std::vector<DiffRow> split;
  std::size_t width = 0;
};

struct GraphRow {
  git::CommitSummary commit;
  std::size_t lane;
  std::vector<std::pair<std::size_t, std::size_t>> continuing;
  std::vector<std::size_t> parents;
};

struct Tab {
  git::Repository& Repo() { return repository.value(); }

  std::optional<git::Repository> repository;
  SavedLocation location;
  SavedTab saved;
  std::string anchor;
  bool restore_anchor = false;
  std::string open_error;
  bool opening = false;
  bool hydrated = false;
  std::string name;
  std::optional<git::RepositorySnapshot> info;
  std::optional<git::StatusSnapshot> status;
  std::optional<git::OperationSnapshot> operation;
  std::vector<git::BranchInfo> branches;
  std::vector<git::RefInfo> tags;
  std::vector<git::RemoteInfo> remotes;
  std::vector<git::StashInfo> stashes;
  std::vector<git::SubmoduleInfo> submodules;
  std::vector<git::WorktreeInfo> worktrees;
  std::shared_ptr<git::HistoryCursor> history;
  std::vector<GraphRow> graph;
  std::vector<git::ObjectId> lanes;
  std::unordered_map<std::string, std::vector<std::string>> decorations;
  std::optional<git::CommitDetails> commit;
  std::shared_ptr<const FileReview> patch;
  git::DiffQuery comparison;
  git::DiffSummary comparison_files;
  git::CommitPrecondition reviewed_commit;
  std::string selected_path;
  std::vector<std::uint32_t> selected_lines;
  std::string last_fetch;
  std::optional<git::Reachability> reachability;
  std::string branch;
  std::string remote = "origin";
  std::string issue;
  WorkspacePage page = WorkspacePage::History;
  TextEditingValue revision_query;
  TextEditingValue message;
  TextEditingValue author_name;
  TextEditingValue author_email;
  TextEditingValue new_branch;
  TextEditingValue new_remote_name;
  TextEditingValue new_remote_url;
  TextEditingValue editor;
  std::string resolved_conflict;
  std::optional<git::OperationVersion> reviewed_conflict;
  std::string base_text;
  std::string ours_text;
  std::string theirs_text;
  std::uint64_t review_generation = 0;
  std::uint64_t conflict_generation = 0;
  unsigned history_generation = 0;
  bool busy = false;
  bool history_busy = false;
  bool history_finished = false;
  bool side_by_side = false;
  bool confirm_abort = false;
  bool edit_identity = false;
  bool edit_repository = false;
  bool show_inspector = false;
  bool show_conflict_base = false;
  bool select_worktree_file = false;
  bool reading_review = false;
  bool can_edit_conflict = false;
};

struct UncertainPush {
  std::string remote;
  std::vector<std::string> destinations;
};

struct Workspace {
  std::vector<std::shared_ptr<Tab>> tabs;
  std::size_t current = 0;
  std::optional<Jobs> transfer;
  std::vector<SavedLocation> recents;
  std::vector<std::string> forgotten_grants;
  std::unordered_map<std::string, std::string> location_errors;
  std::optional<File> session_file;
  std::string pending_session;
  std::string saved_session;
  std::string session_issue;
  std::string relocate;
  bool session_loaded = false;
  bool saving_session = false;
  float history_share = 0.5F;
  float files_width = git_preview::kDefaultFilesWidth;
  git::TransferSnapshot progress;
  std::string transfer_label;
  std::string transfer_remote;
  std::string last_transfer;
  std::unordered_map<std::string, UncertainPush> uncertain_pushes;
  std::string notice;
  TextEditingValue remote;
  TextEditingValue path;
  TextEditingValue username;
  TextEditingValue secret;
  RepositoryAction repository_action = RepositoryAction::Open;
  bool adding_repository = false;
  bool opening_repository = false;
  bool show_path = false;
  bool focus_open = true;
  bool focus_revision = false;
  bool modal_open = false;
  bool menu_open = false;
};

using Revision = State<std::uint64_t>;

void Changed(const Revision& revision) {
  revision += 1;
}

std::string ShortRef(std::string_view name) {
  for (std::string_view prefix : {"refs/heads/", "refs/remotes/", "refs/tags/"}) {
    if (name.starts_with(prefix)) {
      name.remove_prefix(prefix.size());
      break;
    }
  }
  return std::string(name);
}

std::string HeadLabel(const git::HeadState& head) {
  return head.kind == git::HeadKind::Detached && head.target ? head.target->hex.substr(0, 10)
                                                            : ShortRef(head.reference);
}

void ReadDecorations(Tab& tab) {
  tab.decorations.clear();
  for (const auto& branch : tab.branches) {
    auto& labels = tab.decorations[branch.target.hex];
    if (branch.is_head) {
      labels.insert(labels.begin(), ShortRef(branch.name));
    } else {
      labels.push_back(ShortRef(branch.name));
    }
  }
  for (const auto& tag : tab.tags) {
    const auto& target = tag.peeled ? tag.peeled : tag.target;
    if (target) tab.decorations[target->hex].push_back(ShortRef(tag.name));
  }
}

std::string OutcomeText(const git::WriteReport& report, std::string text) {
  if (report.issue) return report.issue->message;
  if (report.status == git::WriteStatus::NeedsResolution) return "Resolve the remaining conflicts";
  if (report.status == git::WriteStatus::NoChange) return "Already up to date";
  if (report.status == git::WriteStatus::Stopped) return "Operation stopped";
  return text;
}

std::string OperationLabel(git::OperationKind kind) {
  switch (kind) {
    case git::OperationKind::Merge: return "Merge in progress";
    case git::OperationKind::Rebase: return "Rebase in progress";
    case git::OperationKind::CherryPick: return "Cherry-pick in progress";
    case git::OperationKind::Revert: return "Revert in progress";
    case git::OperationKind::Other: return "Git operation in progress";
  }
  return {};
}

std::string PhaseLabel(git::TransferPhase phase) {
  switch (phase) {
    case git::TransferPhase::Queued: return "Queued";
    case git::TransferPhase::Connecting: return "Connecting";
    case git::TransferPhase::Receiving: return "Receiving objects";
    case git::TransferPhase::Packing: return "Packing objects";
    case git::TransferPhase::Sending: return "Sending objects";
    case git::TransferPhase::UpdatingReferences: return "Updating references";
    case git::TransferPhase::CheckingOut: return "Checking out files";
    case git::TransferPhase::AwaitingAuthentication: return "Authentication required";
    case git::TransferPhase::Stopping: return "Stopping";
    case git::TransferPhase::Finished: return "Finished";
  }
  return {};
}

template <class T> void UpdateValue(git::GitResult<T> result, T& value, Tab& tab) {
  if (result.Succeeded()) {
    value = std::move(result).Value();
  } else {
    tab.issue = result.Error().message;
  }
}

Task<void> Refresh(std::shared_ptr<Tab> tab, Revision revision) {
  auto info = co_await tab->Repo().InspectAsync();
  if (!info.Succeeded()) {
    tab->issue = info.Error().message;
    Changed(revision);
    co_return;
  }
  tab->info = std::move(info).Value();
  UpdateValue(co_await tab->Repo().ListBranchesAsync(), tab->branches, *tab);
  tab->reachability.reset();
  if (tab->info->head.target && tab->info->upstream) {
    const auto upstream = std::ranges::find(tab->branches, *tab->info->upstream, &git::BranchInfo::name);
    if (upstream != tab->branches.end()) {
      auto counts = co_await tab->Repo().CompareReachabilityAsync(*tab->info->head.target, upstream->target);
      if (counts.Succeeded()) tab->reachability = std::move(counts).Value();
    }
  }
  UpdateValue(co_await tab->Repo().ListTagsAsync(), tab->tags, *tab);
  ReadDecorations(*tab);
  UpdateValue(co_await tab->Repo().ListRemotesAsync(), tab->remotes, *tab);
  if (!tab->remotes.empty() && std::ranges::find(tab->remotes, tab->remote, &git::RemoteInfo::name) ==
                                 tab->remotes.end()) {
    tab->remote = tab->remotes.front().name;
  }
  UpdateValue(co_await tab->Repo().ListStashesAsync(), tab->stashes, *tab);
  UpdateValue(co_await tab->Repo().ListSubmodulesAsync(), tab->submodules, *tab);
  UpdateValue(co_await tab->Repo().ListWorktreesAsync(), tab->worktrees, *tab);
  if (tab->info->work_directory) {
    auto status = co_await tab->Repo().StatusAsync();
    if (status.Succeeded()) {
      tab->status = std::move(status).Value();
    } else {
      tab->issue = status.Error().message;
    }
    auto operation = co_await tab->Repo().ReadOperationAsync();
    if (operation.Succeeded()) {
      auto updated = std::move(operation).Value();
      if (updated && !tab->operation) {
        tab->page = WorkspacePage::Conflicts;
        tab->show_inspector = false;
        ++tab->conflict_generation;
        tab->resolved_conflict.clear();
        tab->reviewed_conflict.reset();
      } else if (!updated && tab->page == WorkspacePage::Conflicts) {
        tab->page = WorkspacePage::Changes;
        tab->show_inspector = false;
      }
      tab->operation = std::move(updated);
    } else {
      tab->issue = operation.Error().message;
    }
  }
  Changed(revision);
}

GraphRow AddToGraph(Tab& tab, git::CommitSummary commit) {
  auto current = std::ranges::find(tab.lanes, commit.id);
  if (current == tab.lanes.end()) {
    tab.lanes.push_back(commit.id);
    current = std::prev(tab.lanes.end());
  }
  const auto lane = static_cast<std::size_t>(current - tab.lanes.begin());
  const auto before = tab.lanes;
  tab.lanes.erase(current);
  auto insertion = lane;
  for (const auto& parent : commit.parents) {
    if (std::ranges::find(tab.lanes, parent) == tab.lanes.end()) {
      tab.lanes.insert(tab.lanes.begin() + insertion++, parent);
    }
  }

  GraphRow row{std::move(commit), lane, {}, {}};
  for (std::size_t i = 0; i < before.size(); ++i) {
    if (i != lane) {
      row.continuing.emplace_back(i, std::ranges::find(tab.lanes, before[i]) - tab.lanes.begin());
    }
  }
  for (const auto& parent : row.commit.parents) {
    row.parents.push_back(std::ranges::find(tab.lanes, parent) - tab.lanes.begin());
  }
  return row;
}

Task<void> ReadHistory(std::shared_ptr<Tab> tab, Revision revision, bool reset) {
  if (!reset && (tab->history_busy || !tab->history || tab->history_finished)) co_return;
  if (reset) {
    ++tab->history_generation;
    tab->history.reset();
    tab->graph.clear();
    tab->lanes.clear();
    tab->history_finished = false;
  }
  const auto generation = tab->history_generation;
  tab->history_busy = true;
  Changed(revision);
  if (reset) {
    git::HistoryQuery query;
    const auto branch = std::ranges::find(tab->branches, tab->branch, &git::BranchInfo::name);
    if (branch != tab->branches.end()) query.starts.push_back(branch->target);
    else tab->branch.clear();
    query.all_refs = query.starts.empty();
    auto result = co_await tab->Repo().OpenHistoryAsync(std::move(query));
    if (generation != tab->history_generation) co_return;
    if (!result.Succeeded()) {
      tab->issue = result.Error().message;
      tab->history_busy = false;
      Changed(revision);
      co_return;
    }
    tab->history = std::make_shared<git::HistoryCursor>(std::move(result).Value());
  }
  auto cursor = tab->history;
  auto page = co_await cursor->NextAsync();
  if (generation != tab->history_generation) co_return;
  if (page.Succeeded()) {
    auto data = std::move(page).Value();
    for (auto& commit : data.commits) tab->graph.push_back(AddToGraph(*tab, std::move(commit)));
    tab->history_finished = data.exhausted;
  } else {
    tab->issue = page.Error().message;
  }
  tab->history_busy = false;
  Changed(revision);
}

Task<void> BrowseCommit(std::shared_ptr<Tab> tab, git::ObjectId id, Revision revision, std::size_t parent = 0);

Task<void> ReadPatch(std::shared_ptr<Tab> tab, git::DiffQuery query, std::string path, Revision revision);

std::shared_ptr<Tab> CurrentTab(const Workspace& workspace) {
  return workspace.current < workspace.tabs.size() ? workspace.tabs[workspace.current] : nullptr;
}

bool ContainsTab(const Workspace& workspace, const std::shared_ptr<Tab>& tab) {
  return std::ranges::find(workspace.tabs, tab) != workspace.tabs.end();
}

std::string NewLocationId() {
  static std::atomic<unsigned> next{0};
  return "lib-git-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
      "-" + std::to_string(next++);
}

void RememberLocation(Workspace& workspace, const SavedLocation& location) {
  std::erase_if(workspace.recents, [&](const SavedLocation& item) { return item.id == location.id; });
  workspace.recents.insert(workspace.recents.begin(), location);
  if (workspace.recents.size() > 15) {
    const auto& removed = workspace.recents.back();
    if (removed.grant) workspace.forgotten_grants.push_back(removed.id);
    workspace.recents.pop_back();
  }
}

void RemoveTab(Workspace& workspace, const std::shared_ptr<Tab>& tab) {
  const auto it = std::ranges::find(workspace.tabs, tab);
  if (it == workspace.tabs.end()) return;
  const auto index = static_cast<std::size_t>(it - workspace.tabs.begin());
  workspace.tabs.erase(it);
  if (workspace.current > index) --workspace.current;
  if (workspace.current >= workspace.tabs.size()) {
    workspace.current = workspace.tabs.empty() ? 0 : workspace.tabs.size() - 1;
  }
  if (tab->location.grant &&
      std::ranges::find(workspace.recents, tab->location.id, &SavedLocation::id) == workspace.recents.end()) {
    workspace.forgotten_grants.push_back(tab->location.id);
  }
}

/// Projects only the navigation intent which can be revalidated on the next launch.
SavedWorkspace WorkspaceSnapshot(const Workspace& workspace) {
  SavedWorkspace snapshot{.locations = workspace.recents, .history_share = workspace.history_share,
                          .files_width = workspace.files_width};
  const auto current = CurrentTab(workspace);
  for (const auto& tab : workspace.tabs) {
    if (tab->location.id.empty()) continue;
    if (std::ranges::find(snapshot.locations, tab->location.id, &SavedLocation::id) == snapshot.locations.end()) {
      snapshot.locations.push_back(tab->location);
    }
    if (tab == current) snapshot.active = tab->location.id;
    if (!tab->hydrated) {
      snapshot.tabs.push_back(tab->saved);
      continue;
    }
    const bool commit = tab->comparison.kind == git::DiffKind::Commit;
    snapshot.tabs.push_back({
      .location = tab->location.id,
      .page = tab->page,
      .branch = tab->branch,
      .commit = commit && tab->comparison.commit ? tab->comparison.commit->hex : std::string{},
      .parent = commit ? tab->comparison.parent_index : 0,
      .path = tab->selected_path,
      .staged = tab->comparison.kind == git::DiffKind::Staged,
      .split = tab->side_by_side,
      .anchor = tab->anchor,
    });
  }
  return snapshot;
}

/// Publishes the latest complete snapshot. Only this task replaces the session file.
Task<void> SaveWorkspace(std::shared_ptr<Workspace> workspace, Revision revision) {
  const auto previous_issue = workspace->session_issue;
  const auto file = *workspace->session_file;
  const auto temporary = file.Parent()->Child(file.Name() + "." + NewLocationId() + ".tmp");
  while (workspace->saved_session != workspace->pending_session) {
    const auto snapshot = workspace->pending_session;
    bool written = false;
    try {
      written = co_await temporary.WriteStringAsync(snapshot);
    } catch (const std::invalid_argument&) {
      workspace->session_issue = "A repository location cannot be stored as text";
    }
    if (!written || !co_await temporary.MoveToAsync(file, true)) {
      co_await temporary.DeleteAsync();
      if (workspace->session_issue.empty()) workspace->session_issue = "Could not save the workspace";
      break;
    }
    workspace->saved_session = snapshot;
    workspace->session_issue.clear();
    // An older grant remains valid until the saved snapshot no longer references it.
    const auto stored = git_preview::DecodeWorkspace(snapshot);
    auto removed = std::exchange(workspace->forgotten_grants, {});
    for (const auto& id : removed) {
      const bool still_saved = !stored || std::ranges::find(stored->locations, id, &SavedLocation::id) != stored->locations.end();
      if (still_saved || std::ranges::any_of(workspace->tabs, [&](const auto& tab) { return tab->location.id == id; })) {
        workspace->forgotten_grants.push_back(id);
      } else {
        const auto result = co_await FileReference::ForgetAsync(id);
        if (!result.Succeeded()) workspace->session_issue = "Could not remove a saved directory grant";
      }
    }
  }
  workspace->saving_session = false;
  if (workspace->session_issue != previous_issue) Changed(revision);
}

/// Rebuilds live Git state before loading any saved selection.
Task<void> HydrateTab(std::shared_ptr<Tab> tab, Revision revision) {
  if (!tab->repository || tab->hydrated || tab->opening) co_return;
  tab->opening = true;
  const auto saved = tab->saved;
  tab->branch = saved.branch;
  tab->side_by_side = saved.split;
  tab->page = saved.page;
  tab->select_worktree_file = saved.page == WorkspacePage::Changes && saved.path.empty();
  tab->anchor = saved.anchor;
  tab->restore_anchor = !saved.anchor.empty() || !saved.commit.empty();
  co_await Refresh(tab, revision);
  if (!tab->info->work_directory) tab->page = WorkspacePage::History;
  if (tab->operation) tab->page = WorkspacePage::Conflicts;
  const auto generation = tab->review_generation;
  if (tab->page == WorkspacePage::History) {
    co_await ReadHistory(tab, revision, true);
    if (generation == tab->review_generation && !tab->graph.empty()) {
      const auto selected = std::ranges::find_if(tab->graph, [&](const GraphRow& row) {
        return row.commit.id.hex == saved.commit;
      });
      const auto& commit = selected != tab->graph.end() ? selected->commit : tab->graph.front().commit;
      const auto parent = selected != tab->graph.end() && saved.parent < commit.parents.size() ? saved.parent : 0;
      co_await BrowseCommit(tab, commit.id, revision, parent);
      if (!saved.path.empty() && std::ranges::any_of(tab->comparison_files.files, [&](const git::FileDelta& file) {
            return (file.new_path.empty() ? file.old_path : file.new_path) == saved.path;
          })) {
        co_await ReadPatch(tab, tab->comparison, saved.path, revision);
      }
    }
  } else if (tab->page == WorkspacePage::Changes && tab->status && !saved.path.empty() &&
             std::ranges::any_of(tab->status->files, [&](const git::FileStatus& file) {
               return !file.conflicted && file.path == saved.path && (saved.staged ? file.staged : file.unstaged);
             })) {
    co_await ReadPatch(tab, {.kind = saved.staged ? git::DiffKind::Staged : git::DiffKind::Worktree},
                       saved.path, revision);
  }
  tab->hydrated = true;
  tab->opening = false;
  Changed(revision);
}

Task<void> AttachRepository(std::shared_ptr<Workspace> workspace, git::Repository repo, Revision revision,
                           std::optional<git::RepositorySource> source = {}, std::string replacing = {}) {
  auto info = co_await repo.InspectAsync();
  if (!info.Succeeded()) {
    workspace->notice = info.Error().message;
    Changed(revision);
    co_return;
  }
  const auto directory = info.Value().git_directory;
  const auto checkout = info.Value().work_directory.value_or(directory);
  const auto existing = std::ranges::find_if(workspace->tabs, [&](const auto& tab) {
    return tab->info && tab->info->git_directory == directory;
  });
  if (existing != workspace->tabs.end()) {
    auto tab = *existing;
    workspace->current = static_cast<std::size_t>(existing - workspace->tabs.begin());
    workspace->adding_repository = false;
    workspace->relocate.clear();
    workspace->notice.clear();
    if (!tab->location.id.empty()) RememberLocation(*workspace, tab->location);
    Changed(revision);
    co_await HydrateTab(std::move(tab), revision);
    co_return;
  }
  auto tab = std::make_shared<Tab>();
  tab->repository = std::move(repo);
  tab->info = std::move(info).Value();
  tab->name = checkout.Name();
  SavedLocation location{NewLocationId(), checkout.Path(), tab->name};
  if (source && std::holds_alternative<FileReference>(*source)) {
    auto saved = co_await std::get<FileReference>(*source).RememberAsync(location.id);
    if (saved.Succeeded()) {
      location.grant = true;
    } else {
      location.id.clear();
      workspace->notice = "Directory access is available for this session only";
    }
  } else {
    const auto known = std::ranges::find_if(workspace->recents, [&](const SavedLocation& item) {
      return !item.grant && item.path == location.path;
    });
    if (known != workspace->recents.end()) location.id = known->id;
  }
  tab->location = std::move(location);
  const auto replacement = std::ranges::find_if(workspace->tabs, [&](const auto& item) {
    return !replacing.empty() && item->location.id == replacing && !item->repository;
  });
  if (replacement != workspace->tabs.end()) {
    tab->saved = (*replacement)->saved;
    *replacement = tab;
    workspace->current = static_cast<std::size_t>(replacement - workspace->tabs.begin());
  } else {
    workspace->tabs.push_back(tab);
    workspace->current = workspace->tabs.size() - 1;
  }
  tab->saved.location = tab->location.id;
  if (!replacing.empty() && replacing != tab->location.id) {
    const auto old = std::ranges::find(workspace->recents, replacing, &SavedLocation::id);
    if (old != workspace->recents.end()) {
      if (old->grant) workspace->forgotten_grants.push_back(old->id);
      workspace->recents.erase(old);
    }
  }
  if (!tab->location.id.empty()) {
    workspace->location_errors.erase(tab->location.id);
    RememberLocation(*workspace, tab->location);
  }
  workspace->adding_repository = false;
  workspace->relocate.clear();
  Changed(revision);
  co_await HydrateTab(tab, revision);
}

Task<void> OpenDirectory(std::shared_ptr<Workspace> workspace, std::shared_ptr<git::GitService> service,
                         git::RepositorySource source, Revision revision, bool initialize = false) {
  if (workspace->opening_repository) co_return;
  workspace->opening_repository = true;
  workspace->notice.clear();
  const auto replacing = workspace->relocate;
  Changed(revision);
  auto result = initialize ? co_await service->InitAsync(source) : co_await service->OpenAsync(source);
  if (result.Succeeded()) {
    co_await AttachRepository(workspace, std::move(result).Value(), revision, std::move(source), replacing);
  } else {
    workspace->notice = result.Error().message;
  }
  workspace->opening_repository = false;
  Changed(revision);
}

Task<void> ChooseDirectory(std::shared_ptr<Workspace> workspace, std::shared_ptr<git::GitService> service,
                           std::shared_ptr<FilePicker> picker, Revision revision) {
  auto selected = co_await picker->OpenDirectoryAsync(true);
  if (selected) {
    co_await OpenDirectory(std::move(workspace), std::move(service), std::move(*selected), revision);
  }
}

Task<void> OpenSavedTab(std::shared_ptr<Workspace> workspace, std::shared_ptr<git::GitService> service,
                        std::shared_ptr<Tab> tab, Revision revision) {
  if (tab->repository || tab->opening || !ContainsTab(*workspace, tab)) co_return;
  tab->opening = true;
  tab->open_error.clear();
  Changed(revision);
  std::optional<git::RepositorySource> source;
  if (tab->location.grant) {
    auto grant = co_await FileReference::RestoreAsync(tab->location.id);
    if (grant.Succeeded()) source = std::move(grant).Value();
    else tab->open_error = "Grant access to this repository again";
  } else {
    try {
      source = File(tab->location.path);
    } catch (const std::invalid_argument&) {
      tab->open_error = "The saved repository path is invalid";
    }
  }
  if (source) {
    auto repo = co_await service->OpenAsync(std::move(*source));
    if (repo.Succeeded()) {
      auto handle = std::move(repo).Value();
      auto info = co_await handle.InspectAsync();
      if (info.Succeeded() && ContainsTab(*workspace, tab)) {
        const auto existing = std::ranges::find_if(workspace->tabs, [&](const auto& other) {
          return other != tab && other->info && other->info->git_directory == info.Value().git_directory;
        });
        if (existing != workspace->tabs.end()) {
          const auto duplicate = *existing;
          const bool active = CurrentTab(*workspace) == tab;
          RemoveTab(*workspace, tab);
          if (active) {
            workspace->current = static_cast<std::size_t>(std::ranges::find(workspace->tabs, duplicate) -
                                                          workspace->tabs.begin());
            co_await HydrateTab(duplicate, revision);
          }
          Changed(revision);
          co_return;
        }
        tab->repository = std::move(handle);
        tab->info = std::move(info).Value();
      } else if (!info.Succeeded()) {
        tab->open_error = info.Error().message;
      }
    } else {
      tab->open_error = repo.Error().message;
    }
  }
  if (!ContainsTab(*workspace, tab)) co_return;
  tab->opening = false;
  if (tab->repository) {
    workspace->location_errors.erase(tab->location.id);
    if (CurrentTab(*workspace) == tab) co_await HydrateTab(tab, revision);
  } else {
    workspace->location_errors[tab->location.id] = tab->open_error;
  }
  Changed(revision);
}

Task<void> LoadWorkspace(std::shared_ptr<Workspace> workspace, std::shared_ptr<git::GitService> service,
                         Revision revision) {
  auto data = co_await workspace->session_file->ReadStringAsync();
  std::optional<SavedWorkspace> saved;
  if (data.Succeeded()) {
    saved = git_preview::DecodeWorkspace(data.Value());
    if (!saved) {
      const auto original = *workspace->session_file;
      const auto recovery = original.Parent()->Child(original.Name() + "." + NewLocationId() + ".recovery");
      const auto preserved = co_await original.MoveToAsync(recovery);
      if (!preserved) workspace->session_file.reset();
      workspace->session_issue = preserved ? "The previous workspace was saved for recovery"
                                           : "The previous workspace could not be read or moved";
    }
  } else if (data.Error().code != IoErrorCode::NotFound) {
    workspace->session_file.reset();
    workspace->session_issue = "Could not read the previous workspace";
  }
  if (saved && workspace->tabs.empty() && !workspace->opening_repository && service->GetCapabilities().local) {
    workspace->recents = std::move(saved->locations);
    workspace->history_share = saved->history_share;
    workspace->files_width = saved->files_width;
    for (auto& view : saved->tabs) {
      const auto location = std::ranges::find(workspace->recents, view.location, &SavedLocation::id);
      auto tab = std::make_shared<Tab>();
      tab->location = *location;
      tab->name = location->name;
      tab->saved = std::move(view);
      if (tab->location.id == saved->active) workspace->current = workspace->tabs.size();
      workspace->tabs.push_back(std::move(tab));
    }
    workspace->recents.resize(std::min<std::size_t>(15, workspace->recents.size()));
    workspace->saved_session = data.Value();
  } else if (!saved && workspace->session_file && workspace->tabs.empty()) {
    workspace->saved_session = git_preview::EncodeWorkspace(WorkspaceSnapshot(*workspace));
  }
  workspace->session_loaded = true;
  Changed(revision);
  const auto pending = workspace->tabs;
  const auto active = CurrentTab(*workspace);
  if (active) co_await OpenSavedTab(workspace, service, active, revision);
  for (const auto& tab : pending) {
    if (tab != active && tab->open_error.empty() && !tab->location.id.empty()) {
      co_await OpenSavedTab(workspace, service, tab, revision);
    }
  }
}

Task<void> ReadPatch(std::shared_ptr<Tab> tab, git::DiffQuery query, std::string path, Revision revision) {
  const auto generation = ++tab->review_generation;
  tab->patch.reset();
  tab->selected_lines.clear();
  tab->selected_path = path;
  tab->comparison = query;
  tab->reading_review = true;
  Changed(revision);
  auto patch = co_await tab->Repo().ReadPatchAsync({std::move(query), std::move(path)});
  if (generation != tab->review_generation) co_return;
  if (patch.Succeeded()) {
    auto review = co_await RunWorker([](git::FileDiff value) {
      return std::make_shared<const FileReview>(std::move(value));
    }, std::move(patch).Value());
    if (generation != tab->review_generation) co_return;
    tab->patch = std::move(review);
  } else {
    tab->issue = patch.Error().message;
  }
  tab->reading_review = false;
  Changed(revision);
}

Task<void> BrowseCommit(std::shared_ptr<Tab> tab, git::ObjectId id, Revision revision, std::size_t parent) {
  const auto generation = ++tab->review_generation;
  tab->patch.reset();
  tab->commit.reset();
  tab->selected_path.clear();
  tab->comparison_files = {};
  tab->reading_review = true;
  tab->comparison = {.kind = git::DiffKind::Commit, .commit = id, .parent_index = parent, .detect_renames = true};
  Changed(revision);
  auto detail = co_await tab->Repo().ReadCommitAsync(std::move(id));
  if (generation != tab->review_generation) co_return;
  if (!detail.Succeeded()) {
    tab->reading_review = false;
    tab->issue = detail.Error().message;
    Changed(revision);
    co_return;
  }
  tab->commit = std::move(detail).Value();
  auto files = co_await tab->Repo().SummarizeDiffAsync(tab->comparison);
  if (generation != tab->review_generation) co_return;
  tab->reading_review = false;
  UpdateValue(std::move(files), tab->comparison_files, *tab);
  Changed(revision);
}

Task<void> ReviewIndex(std::shared_ptr<Tab> tab, Revision revision) {
  const auto generation = ++tab->review_generation;
  tab->select_worktree_file = false;
  tab->reviewed_commit = {};
  tab->commit.reset();
  tab->patch.reset();
  tab->selected_path.clear();
  tab->comparison_files = {};
  tab->reading_review = true;
  tab->comparison = {.kind = git::DiffKind::Staged};
  Changed(revision);
  auto files = co_await tab->Repo().SummarizeDiffAsync(tab->comparison);
  if (generation != tab->review_generation) co_return;
  tab->reading_review = false;
  UpdateValue(std::move(files), tab->comparison_files, *tab);
  tab->reviewed_commit = tab->comparison_files.commit;
  Changed(revision);
}

Task<void> RefreshFromDisk(std::shared_ptr<Tab> tab, Revision revision, bool refresh_review = true) {
  const auto review_generation = tab->review_generation;
  const auto history_generation = tab->history_generation;
  const auto comparison = tab->comparison;
  const auto path = tab->selected_path;
  const auto previous_head = tab->info->head;
  const auto previous_branches = tab->branches;
  const auto previous_tags = tab->tags;
  // A foreground refresh preserves the user's review. Git writes still validate its exact version.
  if (refresh_review) {
    tab->reviewed_commit = {};
    Changed(revision);
  }

  co_await Refresh(tab, revision);
  if (refresh_review && review_generation == tab->review_generation && !path.empty() &&
      comparison.kind != git::DiffKind::Commit) {
    co_await ReadPatch(tab, comparison, path, revision);
  }
  const auto same_tip = [](const auto& left, const auto& right) {
    return left.name == right.name && left.target == right.target;
  };
  if (history_generation == tab->history_generation &&
      (refresh_review || previous_head != tab->info->head ||
       !std::ranges::equal(previous_branches, tab->branches, same_tip) ||
       !std::ranges::equal(previous_tags, tab->tags, same_tip))) {
    co_await ReadHistory(tab, revision, true);
  }
}

Task<void> Change(std::shared_ptr<Tab> tab, Revision revision, Task<git::ChangeOutcome> task,
                  std::string success, bool history = false) {
  tab->busy = true;
  Changed(revision);
  const auto result = co_await std::move(task);
  tab->issue = OutcomeText(result.report, std::move(success));
  tab->reviewed_commit = {};
  tab->busy = false;
  co_await Refresh(tab, revision);
  if (history && result.report.Succeeded()) co_await ReadHistory(tab, revision, true);
}

Task<void> ChangeIndex(std::shared_ptr<Tab> tab, Revision revision, Task<git::ChangeOutcome> task) {
  const auto generation = tab->review_generation;
  const auto comparison = tab->comparison;
  const auto path = tab->selected_path;
  co_await Change(tab, revision, std::move(task), "Updated index");
  if (generation != tab->review_generation || path.empty() || comparison.kind == git::DiffKind::Commit) co_return;
  if (tab->status && std::ranges::any_of(tab->status->files, [&](const git::FileStatus& file) {
        const bool changed = comparison.kind == git::DiffKind::Staged ? file.staged : file.unstaged;
        return changed && (file.path == path || file.previous_path == path);
      })) {
    co_await ReadPatch(tab, comparison, path, revision);
  } else {
    tab->patch.reset();
    tab->selected_path.clear();
    tab->selected_lines.clear();
    Changed(revision);
  }
}

Task<void> ApplyEdits(std::shared_ptr<Tab> tab, Revision revision, std::shared_ptr<const FileReview> patch,
                     std::optional<std::uint32_t> hunk) {
  if (!patch || patch != tab->patch || !patch->diff.staging.IsValid()) co_return;
  git::PatchSelectionRequest request;
  request.action = tab->comparison.kind == git::DiffKind::Staged ? git::PatchAction::Unstage
                                                               : git::PatchAction::Stage;
  request.snapshot = patch->diff.staging;
  if (hunk) request.hunks = {*hunk};
  else request.lines = tab->selected_lines;
  co_await ChangeIndex(tab, revision, tab->Repo().ApplySelectionAsync(std::move(request)));
}

Task<void> Commit(std::shared_ptr<Tab> tab, Revision revision, bool amend) {
  if (!tab->reviewed_commit.IsValid()) co_return;
  tab->busy = true;
  Changed(revision);
  git::CommitRequest request{tab->reviewed_commit, tab->message.text};
  const auto result = amend ? co_await tab->Repo().AmendAsync(std::move(request))
                            : co_await tab->Repo().CommitAsync(std::move(request));
  tab->issue = OutcomeText(result.report, amend ? "Commit amended" : "Commit created");
  tab->reviewed_commit = {};
  if (result.created_commit && result.report.Succeeded()) tab->message = {};
  tab->busy = false;
  co_await Refresh(tab, revision);
  co_await ReadHistory(tab, revision, true);
}

Task<void> Integrate(std::shared_ptr<Tab> tab, Revision revision, Task<git::IntegrationOutcome> task) {
  tab->busy = true;
  Changed(revision);
  const auto result = co_await std::move(task);
  tab->issue = OutcomeText(result.report, "Integration completed");
  tab->busy = false;
  tab->reviewed_commit = {};
  co_await Refresh(tab, revision);
  if (result.report.Succeeded()) co_await ReadHistory(tab, revision, true);
}

struct ConflictText {
  std::string text;
  bool editable = true;
};

Task<ConflictText> ReadConflictSide(git::Repository repo, std::optional<git::ConflictSide> side) {
  if (!side) co_return ConflictText{"Not present"};
  if (side->mode != 0100644 && side->mode != 0100755) co_return ConflictText{"Special file · " + side->id.hex, false};
  auto result = co_await repo.ReadBlobAsync(side->id, 256 * 1024);
  if (!result.Succeeded()) co_return ConflictText{result.Error().message, false};
  const auto& bytes = result.Value();
  if (std::ranges::find(bytes, std::byte{0}) != bytes.end()) {
    co_return ConflictText{"Binary file · " + side->id.hex, false};
  }
  co_return ConflictText{
      bytes.empty() ? std::string{} : std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())};
}

Task<void> LoadConflict(std::shared_ptr<Tab> tab, git::ConflictEntry conflict, git::OperationVersion version,
                        Revision revision) {
  if (!tab->operation || tab->operation->version != version) co_return;
  const auto generation = ++tab->conflict_generation;
  tab->resolved_conflict = conflict.path;
  tab->reviewed_conflict.reset();
  Changed(revision);
  auto base = co_await ReadConflictSide(tab->Repo(), conflict.base);
  auto ours = co_await ReadConflictSide(tab->Repo(), conflict.ours);
  auto theirs = co_await ReadConflictSide(tab->Repo(), conflict.theirs);
  if (generation != tab->conflict_generation) co_return;
  tab->base_text = std::move(base.text);
  tab->ours_text = std::move(ours.text);
  tab->theirs_text = std::move(theirs.text);
  tab->can_edit_conflict = base.editable && ours.editable && theirs.editable;
  tab->editor = TextEditingValue::FromText(conflict.ours && tab->can_edit_conflict ? tab->ours_text : std::string{});
  tab->reviewed_conflict = version;
  Changed(revision);
}

Task<void> Resolve(std::shared_ptr<Tab> tab, Revision revision, std::string path,
                   git::OperationVersion version, git::Resolution resolution) {
  if (!tab->operation || !tab->reviewed_conflict || *tab->reviewed_conflict != version ||
      tab->operation->version != version || tab->resolved_conflict != path) co_return;
  git::ResolveRequest request{std::move(version), std::move(path), resolution};
  if (resolution == git::Resolution::Content) {
    if (!tab->can_edit_conflict) co_return;
    const auto* first = reinterpret_cast<const std::byte*>(tab->editor.text.data());
    request.content.assign(first, first + tab->editor.text.size());
  }
  co_await Change(tab, revision, tab->Repo().ResolveConflictAsync(std::move(request)), "Conflict resolved");
}

template <class Outcome>
Task<void> TrackTransfer(std::shared_ptr<Workspace> workspace, std::shared_ptr<Tab> tab,
                         git::Transfer<Outcome> transfer, TaskScope app_tasks, Revision revision) {
  auto watch = transfer.Observe(app_tasks, [workspace, revision](git::TransferSnapshot update) {
    if (workspace->progress.id != update.id) return;
    workspace->progress = std::move(update);
    Changed(revision);
  });
  const auto result = co_await transfer.WaitAsync();
  workspace->notice = OutcomeText(result.report, workspace->transfer_label + " completed");
  if constexpr (std::same_as<Outcome, git::FetchOutcome>) {
    if (tab && result.report.Succeeded()) tab->last_fetch = "Fetched " + workspace->transfer_remote;
  } else if constexpr (std::same_as<Outcome, git::PullOutcome>) {
    if (result.fetch && result.fetch->report.Succeeded()) {
      if (tab) tab->last_fetch = "Fetched " + workspace->transfer_remote;
      if (!result.report.Succeeded()) workspace->notice = "Fetched successfully · " + workspace->notice;
    }
  }
  if constexpr (std::same_as<Outcome, git::PushOutcome>) {
    for (const auto& ref : result.references) {
      if (ref.status == git::RefPushStatus::Unknown) {
        workspace->notice += " · remote result unknown for " + ShortRef(ref.destination);
        if (tab) {
          auto& pending = workspace->uncertain_pushes[tab->info->git_directory.Path()];
          pending.remote = workspace->transfer_remote;
          pending.destinations.push_back(ref.destination);
        }
      } else if (ref.status == git::RefPushStatus::Rejected) {
        workspace->notice += " · rejected " + ShortRef(ref.destination);
      }
    }
  }
  workspace->last_transfer = workspace->transfer_label + "\n" + workspace->notice;
  workspace->transfer.reset();
  workspace->progress = {};
  workspace->secret = {};
  Changed(revision);
  if constexpr (std::same_as<Outcome, git::CloneOutcome>) {
    if (result.repository) co_await AttachRepository(workspace, *result.repository, revision);
  } else if (tab) {
    co_await Refresh(tab, revision);
    co_await ReadHistory(tab, revision, true);
  }
}

template <class Outcome>
void StartTransfer(std::shared_ptr<Workspace> workspace, std::shared_ptr<Tab> tab,
                   git::GitResult<git::Transfer<Outcome>> result, std::string label, TaskScope app_tasks,
                   Revision revision) {
  if (!result.Succeeded()) {
    workspace->notice = result.Error().message;
    Changed(revision);
    return;
  }
  auto transfer = std::move(result).Value();
  workspace->progress = transfer.Snapshot();
  workspace->transfer = transfer;
  workspace->transfer_remote = tab ? tab->remote : std::string{};
  workspace->transfer_label = tab ? tab->name + " · " + label + " · " + tab->remote : std::move(label);
  workspace->notice.clear();
  Changed(revision);
  app_tasks.Launch(TrackTransfer(workspace, std::move(tab), std::move(transfer), app_tasks, revision));
}

Task<void> CheckPush(std::shared_ptr<Workspace> workspace, std::shared_ptr<Tab> tab,
                     std::shared_ptr<git::GitService> service, Revision revision) {
  const auto directory = tab->info->git_directory.Path();
  const auto push = workspace->uncertain_pushes.find(directory);
  if (tab->busy || push == workspace->uncertain_pushes.end()) co_return;
  const auto request = push->second;
  tab->busy = true;
  Changed(revision);
  const auto result = co_await service->ReadRemoteRefsAsync(tab->Repo(), request.remote);
  if (!result.Succeeded()) {
    tab->issue = result.Error().message;
  } else {
    tab->issue = request.remote;
    for (const auto& name : request.destinations) {
      const auto ref = std::ranges::find(result.Value(), name, &git::RefInfo::name);
      const auto id = ref == result.Value().end() || !ref->target ? "absent" : ref->target->hex.substr(0, 12);
      tab->issue += " · " + ShortRef(name) + " " + id;
    }
    workspace->uncertain_pushes.erase(directory);
  }
  tab->busy = false;
  Changed(revision);
}

constexpr Color kWindow = Color::Rgb(250, 251, 253);
constexpr Color kToolbar = Color::Rgb(246, 248, 252);
constexpr Color kSidebar = Color::Rgb(248, 250, 253);
constexpr Color kSurface = Color::White();
constexpr Color kElevated = Color::Rgb(239, 243, 249);
constexpr Color kBorder = Color::Rgb(225, 231, 240);
constexpr Color kText = Color::Rgb(23, 34, 54);
constexpr Color kMuted = Color::Rgb(80, 96, 120);
constexpr Color kAccent = Color::Rgb(36, 82, 205);
constexpr Color kSelection = Color::Rgb(233, 240, 255);
constexpr Color kSelectedLine = Color::Rgb(220, 233, 255);
constexpr Color kHunk = Color::Rgb(238, 243, 255);
constexpr Color kWarning = Color::Rgb(121, 72, 19);
constexpr Color kWarningSurface = Color::Rgb(255, 247, 230);
constexpr Color kNegative = Color::Rgb(157, 36, 51);
constexpr Color kPositive = Color::Rgb(17, 99, 62);
constexpr Color kAdded = Color::Rgb(231, 247, 237);
constexpr Color kRemoved = Color::Rgb(253, 235, 237);
constexpr float kPageHeadingHeight = 50.0F;
constexpr float kHistoryRowHeight = 33.0F;
constexpr float kChangeFileRowHeight = 36.0F;

Indication HoverFeedback() {
  return {
      .hover = IndicationLayer{.fill = Color::Rgb(21, 81, 153, 0.08F)},
      .press = IndicationLayer{.fill = Color::Rgb(21, 81, 153, 0.14F)},
  };
}

ButtonStyle WorkspaceButtonStyle(bool primary = false) {
  return {
      .background = primary ? kAccent : kToolbar,
      .label_style = TextStyle{Font::System(12.0F).WithWeight(FontWeight::SemiBold),
                               primary ? Color::White() : kText},
      .disabled_background = kWindow,
      .disabled_label = Color::Rgb(126, 138, 154),
      .padding = EdgeInsets::Symmetric(11.0F, 6.0F),
      .minimum_height = 30.0F,
      .corner_radii = CornerRadii{6.0F},
      .indication = HoverFeedback(),
  };
}

ThemeDefinition WorkspaceTheme() {
  auto theme = FlatLightThemeSpec();
  theme.colors.primary = kAccent;
  theme.colors.on_primary = Color::White();
  theme.colors.primary_container = kSelection;
  theme.colors.on_primary_container = kAccent;
  theme.colors.background = kWindow;
  theme.colors.surface = kSurface;
  theme.colors.surface_container_low = kSidebar;
  theme.colors.surface_container = kToolbar;
  theme.colors.surface_container_high = kElevated;
  theme.colors.surface_container_highest = kBorder;
  theme.colors.on_surface = kText;
  theme.colors.on_surface_variant = kMuted;
  theme.colors.outline = kBorder;
  theme.colors.error = kNegative;
  theme.typography.body_medium = 13.0F;
  theme.typography.label_large = 12.0F;
  theme.shapes.small = 6.0F;
  theme.shapes.medium = 9.0F;
  theme.interactions.focus_ring = FocusRing{kAccent, 2.0F, 0.0F};
  auto definition = FlatThemeDefinition(theme);
  definition.Set(WorkspaceButtonStyle());
  definition.Set(DividerStyle{.color = kBorder, .thickness = 1.0F});
  auto menu = MenuStyle::Default();
  menu.background = kSurface;
  menu.foreground = kText;
  menu.minimum_width = 246.0F;
  menu.minimum_item_height = 29.0F;
  menu.item_padding = EdgeInsets::Symmetric(9.0F, 5.0F);
  menu.item_indication = HoverFeedback();
  menu.separator_color = kBorder;
  menu.separator_mode = MenuSeparatorMode::BetweenSections;
  definition.Set(menu);
  auto dialog = DialogStyle::Default();
  dialog.minimum_width = 390.0F;
  dialog.scrim = Color::Rgb(21, 34, 56, 0.30F);
  dialog.title_style = TextStyle{Font::System(19.0F).WithWeight(FontWeight::SemiBold), kText};
  dialog.message_style = TextStyle{Font::System(13.0F), kMuted};
  dialog.content_padding = EdgeInsets::All(22.0F);
  dialog.action_separator_color = kBorder;
  dialog.action_separator_thickness = 1.0F;
  dialog.positive_action_background = kAccent;
  definition.Set(dialog);
  definition.Set(SegmentedButtonStyle{
      .background = kToolbar, .selected_background = kSurface,
      .label_style = TextStyle{Font::System(10.5F).WithWeight(FontWeight::SemiBold), kMuted},
      .selected_label = kAccent, .border = Border{kBorder, 1.0F},
      .selected_border = Border{Color::Rgb(193, 208, 241), 1.0F},
      .padding = EdgeInsets::Symmetric(8.0F, 3.0F), .minimum_segment_width = 48.0F,
      .minimum_height = 26.0F, .corner_radii = CornerRadii{6.0F}, .indication = HoverFeedback(),
  });
  return definition;
}

Text SingleLine(std::string value, Color color = kText, float size = 13.0F,
                FontWeight weight = FontWeight::Regular) {
  return Text(std::move(value)).Style(TextStyle{Font::System(size).WithWeight(weight), color})
      .LineLimit(1).Overflow(TextOverflow::Ellipsis);
}

Text CodeLabel(std::string value, Color color = kMuted, float size = 11.0F) {
  return Text(std::move(value)).Style(TextStyle{Font::Monospace(size), color})
      .LineLimit(1).Overflow(TextOverflow::MiddleEllipsis);
}

View Badge(std::string text, Color color = kAccent, Color fill = kSelection) {
  return SingleLine(std::move(text), color, 10.0F, FontWeight::SemiBold)
      .With(Padding(EdgeInsets::Symmetric(6.0F, 3.0F)), Background(fill), CornerRadius(4.0F));
}

enum class WorkspaceIcon { Branch, History, Changes, Conflict, Refresh, Fetch, Push, File, Folder, Search, Copy };

/// Draws a small stroke icon within its fixed logical bounds.
/// @param kind Shape used by the control or row.
/// @param tint Color shared with the control's label or secondary text.
View Glyph(WorkspaceIcon kind, Color tint = kMuted) {
  return Canvas([=](PaintContext& paint, Size) {
    const StrokeStyle stroke{.width = 1.25F};
    const auto line = [&](Point from, Point to) { paint.DrawLine(from, to, tint, stroke); };
    const auto outline = [&](std::initializer_list<Point> points) {
      auto last = points.begin();
      for (auto next = std::next(last); next != points.end(); last = next++) line(*last, *next);
    };
    const auto dot = [&](Point point) {
      paint.DrawCircle(point, 2.1F, tint);
      paint.DrawCircle(point, 0.9F, kSurface);
    };
    switch (kind) {
      case WorkspaceIcon::Branch:
        line({5,4}, {5,13}); outline({{5,9}, {8,7}, {11,7}});
        dot({5,4}); dot({5,13}); dot({12,7}); break;
      case WorkspaceIcon::History:
        paint.DrawArc({8,8}, 5.8F, 0.0F, 6.28319F, tint, stroke);
        outline({{8,4.5F}, {8,8}, {10.5F,9.3F}}); break;
      case WorkspaceIcon::Changes:
      case WorkspaceIcon::File:
        outline({{4,2}, {9,2}, {13,6}, {13,14}, {4,14}, {4,2}});
        outline({{9,2}, {9,6}, {13,6}});
        if (kind == WorkspaceIcon::Changes) { line({6,8.5F}, {11,8.5F}); line({6,11}, {10,11}); }
        break;
      case WorkspaceIcon::Conflict:
        outline({{8,2.5F}, {14,13.5F}, {2,13.5F}, {8,2.5F}});
        line({8,6}, {8,9}); paint.DrawCircle({8,11.2F}, 0.7F, tint); break;
      case WorkspaceIcon::Refresh:
        outline({{13,6}, {11,3}, {6,2.5F}, {3,5}, {3,8}});
        outline({{3,10}, {5,13}, {10,13.5F}, {13,11}, {13,8}});
        outline({{10,6}, {13,6}, {13,3}}); outline({{3,13}, {3,10}, {6,10}}); break;
      case WorkspaceIcon::Fetch:
        line({8,3}, {8,12}); outline({{4.5F,9}, {8,12.5F}, {11.5F,9}}); break;
      case WorkspaceIcon::Push:
        line({8,13}, {8,4}); outline({{4.5F,7}, {8,3.5F}, {11.5F,7}}); break;
      case WorkspaceIcon::Folder:
        outline({{2,12.5F}, {2,4}, {7,4}, {8.5F,6}, {14,6}, {14,12.5F}, {2,12.5F}}); break;
      case WorkspaceIcon::Search:
        paint.DrawArc({7,7}, 3.8F, 0.0F, 6.28319F, tint, stroke);
        line({9.8F,9.8F}, {13,13}); break;
      case WorkspaceIcon::Copy:
        outline({{6,5}, {13,5}, {13,14}, {6,14}, {6,5}});
        outline({{10,3}, {3,3}, {3,12}}); break;
    }
  }).With(Frame{.width = 16.0F, .height = 16.0F});
}

View ToolbarAction(std::string label, WorkspaceIcon icon, std::function<void()> action, bool enabled = true) {
  const auto color = enabled ? kText : Color::Rgb(126, 138, 154);
  return Row {
    Glyph(icon, color),
    SingleLine(label, color, 12.0F, FontWeight::SemiBold),
  }.With(Frame{.height = 32.0F}, Padding(EdgeInsets::Symmetric(9.0F, 0.0F)), Spacing(6.0F),
         Background(kSurface), Border{.color = enabled ? kBorder : kToolbar}, CornerRadius(6.0F),
         CrossAlign(CrossAxisAlignment::Center), HoverFeedback(), Enabled(enabled), Focusable(true),
         Semantics{.role = SemanticRole::Button, .label = label}).OnClick(std::move(action));
}

View PrimaryAction(std::string label, std::function<void()> on_click, bool enabled = true,
                   std::optional<ViewHandle> focus = {}) {
  ThemeDefinition style;
  style.Set(WorkspaceButtonStyle(true));
  auto button = Button(std::move(label)).OnClick(std::move(on_click)).With(Enabled(enabled));
  return focus ? Theme(style, std::move(button).With(*focus)) : Theme(style, std::move(button));
}

/// Styles the compact actions in the commit and diff panes.
/// @param primary Whether the action uses the blue primary style.
/// @param selected Whether the action represents the current comparison parent.
ButtonStyle ReviewButtonStyle(bool primary = false, bool selected = false) {
  auto style = WorkspaceButtonStyle(primary);
  style.background = primary ? kAccent : selected ? kSelection : kSurface;
  style.label_style = TextStyle{Font::System(11.0F).WithWeight(FontWeight::SemiBold),
                               primary ? Color::White() : selected ? kAccent : kText};
  style.disabled_background = primary ? Color::Rgb(222, 232, 252) : kToolbar;
  style.disabled_label = primary ? Color::Rgb(74, 102, 164) : kMuted;
  style.padding = EdgeInsets::Symmetric(8.0F, 3.0F);
  style.minimum_height = 24.0F;
  style.corner_radii = CornerRadii{5.0F};
  return style;
}

/// Presents an action with the review panes' compact button geometry.
/// @param label Visible and accessible action name.
/// @param action Action invoked on activation.
/// @param enabled Whether the action is currently available.
/// @param primary Whether the action is the primary action for its section.
View ReviewButton(std::string label, std::function<void()> action, bool enabled = true, bool primary = false) {
  ThemeDefinition style;
  style.Set(ReviewButtonStyle(primary));
  return Theme(style, Button(std::move(label)).OnClick(std::move(action))
      .With(Enabled(enabled), CornerRadius(5.0F), Border{.color = primary && enabled ? kAccent : kBorder}));
}

/// Styles an outlined field while retaining its label for accessibility.
/// @param fill Field background color.
/// @param height Minimum field height in logical units.
/// @param padding Space inside the field border.
/// @param font_size Size of the input and placeholder text.
ThemeDefinition ReviewFieldStyle(Color fill, float height, EdgeInsets padding, float font_size) {
  auto field = TextFieldStyle::Default();
  field.show_label = false;
  field.text_style = TextStyle{Font::System(font_size), kText};
  field.placeholder_style = TextStyle{Font::System(font_size), kMuted};
  field.padding = padding;
  field.outlined = {.background = fill, .border = kBorder, .hovered_border = Color::Rgb(180, 196, 220),
                    .focused_border = kAccent, .corner_radii = CornerRadii{7.0F}, .minimum_height = height};
  ThemeDefinition style;
  style.Set(field);
  return style;
}

/// Draws the file status in a small square marker.
/// @param label Git status abbreviation.
/// @param tint Color of the status text.
/// @param fill Background color of the marker.
View FileFlag(std::string label, Color tint, Color fill) {
  return Row {CodeLabel(std::move(label), tint, 10.0F)}
      .With(Frame{.width = 17.0F, .height = 17.0F}, MainAlign(MainAxisAlignment::Center),
            CrossAlign(CrossAxisAlignment::Center), Background(fill), CornerRadius(4.0F));
}

View RepositoryGlyph(float size) {
  return Canvas([](PaintContext& paint, Size extent) {
    const float f = extent.width / 32.0F;
    const StrokeStyle stroke{.width = 1.8F * f};
    paint.DrawLine({10 * f, 8 * f}, {10 * f, 24 * f}, kAccent, stroke);
    paint.DrawLine({10 * f, 17 * f}, {21 * f, 12 * f}, kAccent, stroke);
    paint.DrawCircle({10 * f, 8 * f}, 2.6F * f, kAccent);
    paint.DrawCircle({10 * f, 24 * f}, 2.6F * f, kAccent);
    paint.DrawCircle({21 * f, 12 * f}, 2.6F * f, kAccent);
  }).With(Frame{.width = size, .height = size}, Background(kSelection), CornerRadius(size * 0.22F));
}

View LabelSection(std::string title, std::size_t count) {
  return Row {
    SingleLine(std::move(title), kMuted, 10.0F, FontWeight::SemiBold).With(Grow()),
    CodeLabel(std::to_string(count)),
  }.With(Padding(EdgeInsets::Symmetric(10.0F, 6.0F)), CrossAlign(CrossAxisAlignment::Center));
}

View SelectableRow(std::string label, bool selected, std::function<void()> action, View trailing = {}, View leading = {}) {
  return Row {
    leading,
    SingleLine(label, selected ? kAccent : kText, 12.0F, selected ? FontWeight::SemiBold : FontWeight::Regular)
        .With(Grow()),
    trailing,
  }.With(Padding(EdgeInsets::Symmetric(10.0F, 7.0F)), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center),
         Background(selected ? kSelection : Color::Rgb(0, 0, 0, 0.0F)), CornerRadius(5.0F), HoverFeedback(),
         Focusable(true),
         Semantics{.role = SemanticRole::Button, .label = label, .selected = selected})
      .OnClick(std::move(action));
}

std::string CommitDate(std::chrono::sys_seconds time, bool include_year = true) {
  constexpr std::array months{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const std::chrono::year_month_day date{std::chrono::floor<std::chrono::days>(time)};
  return std::string(months[static_cast<unsigned>(date.month()) - 1]) + " " +
         std::to_string(static_cast<unsigned>(date.day())) +
         (include_year ? ", " + std::to_string(static_cast<int>(date.year())) : "");
}

Task<void> BrowseBranch(std::shared_ptr<Tab> tab, std::string ref, Revision revision);

void SelectPage(const std::shared_ptr<Tab>& tab, WorkspacePage page, TaskScope tasks, Revision revision) {
  if (tab->page != page) {
    ++tab->review_generation;
    tab->reading_review = false;
    tab->patch.reset();
    tab->commit.reset();
    tab->selected_path.clear();
    tab->comparison_files = {};
    tab->reviewed_commit = {};
    tab->page = page;
    tab->select_worktree_file = page == WorkspacePage::Changes;
    if (page == WorkspacePage::History) {
      if (tab->graph.empty()) tasks.Launch(BrowseBranch(tab, tab->branch, revision));
      else tasks.Launch(BrowseCommit(tab, tab->graph.front().commit.id, revision));
    }
  }
  tab->show_inspector = false;
  Changed(revision);
}

Task<void> FindRevision(std::shared_ptr<Tab> tab, Revision revision, bool show_inspector) {
  auto result = co_await tab->Repo().ResolveRevisionAsync(tab->revision_query.text);
  if (!result.Succeeded()) {
    tab->issue = result.Error().message;
    Changed(revision);
    co_return;
  }
  tab->show_inspector = show_inspector;
  tab->issue.clear();
  co_await BrowseCommit(tab, std::move(result).Value(), revision);
}

View GraphGraphic(std::shared_ptr<Tab> tab, std::size_t index, float width, bool compact, bool selected) {
  return Canvas([tab, index, compact, selected](PaintContext& paint, Size size) {
    if (index >= tab->graph.size()) return;
    const auto& row = tab->graph[index];
    constexpr Color colors[]{kAccent, Color::Rgb(121, 76, 176), Color::Rgb(23, 126, 120),
                             Color::Rgb(151, 59, 96), Color::Rgb(156, 91, 28)};
    const auto x = [size, compact](std::size_t lane) {
      return std::min(size.width - 6.0F, (compact ? 15.0F : 25.0F) + 12.0F * static_cast<float>(lane));
    };
    const auto color = [&](std::size_t lane) { return colors[lane % std::size(colors)]; };
    const float middle = size.height / 2;
    const StrokeStyle stroke{.width = 1.5F};
    const auto edge = [&](float from, float from_y, float to, float to_y, Color tint) {
      if (from == to) {
        paint.DrawLine({from, from_y}, {to, to_y}, tint, stroke);
      } else {
        Path path;
        path.MoveTo({from, from_y}).CubicTo({from, (from_y + to_y) / 2},
                                          {to, (from_y + to_y) / 2}, {to, to_y});
        paint.StrokePath(std::move(path), tint, stroke);
      }
    };
    for (const auto& [from, to] : row.continuing) {
      edge(x(from), 0, x(to), size.height, color(from));
    }
    const auto center = x(row.lane);
    if (index != 0) edge(center, 0, center, middle, color(row.lane));
    for (auto parent : row.parents) {
      edge(center, middle, x(parent), size.height, color(row.lane));
    }
    paint.DrawCircle({center, middle}, 4.5F, color(row.lane));
    if (!selected) paint.DrawCircle({center, middle}, 2.2F, kSurface);
  }).With(Frame{.width = width, .height = kHistoryRowHeight});
}

[[huxerui::composable]]
View HistoryPane(std::shared_ptr<Tab> tab, TaskScope tasks, Revision revision, bool compact, bool detailed, ViewHandle revision_focus) {
  static_cast<void>(static_cast<std::uint64_t>(revision));
  const auto scroll = UseScrollController();
  const auto list_focus = UseViewHandle();
  const auto query_revision = UseState(std::uint64_t{0});
  static_cast<void>(static_cast<std::uint64_t>(query_revision));
  const auto metrics = scroll.Metrics();
  const float graph_column = compact ? 66.0F : 89.0F;
  constexpr float author_column = 111.0F;
  constexpr float date_column = 84.0F;
  constexpr float hash_column = 75.0F;
  const bool needs_page = scroll.IsConnected() && !tab->graph.empty() &&
                          metrics.maximum_offset - metrics.offset < 260.0F;
  Lifecycle([=] {
    if (needs_page && !tab->history_busy && !tab->history_finished) {
      tasks.Launch(ReadHistory(tab, revision, false));
    }
  }, needs_page, tab->graph.size(), tab->history_finished);
  const bool connected = scroll.IsConnected();
  Lifecycle([=] {
    if (!connected || tab->graph.empty()) return;
    if (tab->restore_anchor) {
      tab->restore_anchor = false;
      const auto anchor = std::ranges::find_if(tab->graph, [&](const GraphRow& row) {
        return tab->anchor.empty() ? tab->comparison.commit == row.commit.id : row.commit.id.hex == tab->anchor;
      });
      const auto index = anchor == tab->graph.end() ? 0 : static_cast<std::size_t>(anchor - tab->graph.begin());
      if (scroll.ScrollToItem(index)) return;
    }
    const auto index = std::min(tab->graph.size() - 1, static_cast<std::size_t>(metrics.offset / kHistoryRowHeight));
    if (tab->anchor != tab->graph[index].commit.id.hex) {
      tab->anchor = tab->graph[index].commit.id.hex;
      Changed(revision);
    }
  }, tab, connected, metrics.offset, tab->graph.size());

  const auto move = [=](const KeyEvent& event) {
    if (event.type != KeyEventType::Down || event.modifiers != KeyModifiers{} ||
        (event.key != Key::ArrowUp && event.key != Key::ArrowDown) || tab->graph.empty()) return false;
    const auto selected = std::ranges::find_if(tab->graph, [&](const GraphRow& row) {
      return tab->comparison.commit == row.commit.id;
    });
    auto next = selected == tab->graph.end() ? 0 : static_cast<std::size_t>(selected - tab->graph.begin());
    if (event.key == Key::ArrowDown) next = std::min(next + 1, tab->graph.size() - 1);
    else if (next) --next;
    const auto start = static_cast<float>(next) * kHistoryRowHeight;
    if (start < metrics.offset) scroll.ScrollToItem(next, ScrollAlignment::Start);
    else if (start + kHistoryRowHeight > metrics.offset + metrics.viewport_extent) scroll.ScrollToItem(next, ScrollAlignment::End);
    list_focus.RequestFocus(true);
    tasks.Launch(BrowseCommit(tab, tab->graph[next].commit.id, revision));
    return true;
  };

  const auto find = [=] {
    if (!tab->revision_query.text.empty()) tasks.Launch(FindRevision(tab, revision, compact));
  };
  constexpr float search_width = 179.0F;
  constexpr float search_height = 30.0F;
  const auto search_style = ReviewFieldStyle(kSidebar, search_height,
      EdgeInsets{.top = 4.0F, .right = compact ? 8.0F : 47.0F, .bottom = 4.0F, .left = compact ? 8.0F : 28.0F}, 11.5F);
  auto input = TextField(tab->revision_query).Label("Go to revision").Placeholder("Go to revision")
      .Variant(TextFieldVariant::Outlined).InputConfiguration({.action = TextInputAction::Search})
      .OnChanged([=](const TextEditingValue& value) { tab->revision_query = value; Changed(query_revision); })
      .OnSubmitted(find)
      .With(revision_focus, Frame{.width = compact ? std::nullopt : std::optional{search_width}, .height = search_height},
            Grow(compact ? 1.0F : 0.0F)).Key("revision-input");
#if defined(__APPLE__)
  constexpr std::string_view revision_shortcut = "⌘L";
#else
  constexpr std::string_view revision_shortcut = "Ctrl+L";
#endif
  View search = std::move(input);
  if (!compact) {
    search = Stack {
      std::move(search),
      Row {
        Glyph(WorkspaceIcon::Search),
        Spacer(),
        CodeLabel(std::string(revision_shortcut), kMuted, 9.0F)
            .With(Padding(EdgeInsets::Symmetric(4.0F, 2.0F)), Background(kSurface), CornerRadius(3.0F),
                  Border{.color = kBorder}),
      }.With(Frame{.width = search_width, .height = search_height}, Padding(EdgeInsets::Symmetric(7.0F, 0.0F)),
             CrossAlign(CrossAxisAlignment::Center)),
    }.With(Frame{.width = search_width, .height = search_height}).Key("revision-search");
  }
  search = Theme(search_style, std::move(search));
  View scope = Row {
    Glyph(WorkspaceIcon::Branch),
    SingleLine(tab->branch.empty() ? "All refs" : ShortRef(tab->branch), kMuted, 11.5F, FontWeight::SemiBold)
        .With(Frame{.max_width = 155.0F}),
    tab->branch.empty() ? View{} : SingleLine("×", kMuted, 13.0F),
  }.With(Spacing(4.0F), Padding(EdgeInsets::Symmetric(5.0F, 3.0F)), CornerRadius(5.0F),
         CrossAlign(CrossAxisAlignment::Center));
  if (!tab->branch.empty()) {
    scope = std::move(scope).With(HoverFeedback(), Focusable(true),
                                  Semantics{.role = SemanticRole::Button, .label = "Show all refs"})
        .OnClick([=] { tasks.Launch(BrowseBranch(tab, {}, revision)); });
  }
  auto columns = Row {
    SingleLine("Graph", kMuted, 10.5F, FontWeight::SemiBold).With(Frame{.width = graph_column - 14.0F}),
    SingleLine("Commit", kMuted, 10.5F, FontWeight::SemiBold).With(Grow()),
    detailed ? SingleLine("Author", kMuted, 10.5F, FontWeight::SemiBold).With(Frame{.width = author_column}) : View{},
    detailed ? SingleLine("Date", kMuted, 10.5F, FontWeight::SemiBold).With(Frame{.width = date_column}) : View{},
    SingleLine("Hash", kMuted, 10.5F, FontWeight::SemiBold).With(Frame{.width = hash_column}),
  }.With(Frame{.height = 27.0F}, Padding(EdgeInsets{.right = 10.0F, .left = 14.0F}),
         Background(kSidebar), CrossAlign(CrossAxisAlignment::Center));
  return Column {
    Row {
      SingleLine("History", kText, 25.0F, FontWeight::Bold),
      scope,
      Spacer(),
      !compact ? search : View{},
    }.With(Frame{.height = kPageHeadingHeight}, Spacing(10.0F), Padding(EdgeInsets::Symmetric(16.0F, 0.0F)),
           CrossAlign(CrossAxisAlignment::Center)),
    compact ? Row {search, ReviewButton("Go", find, !tab->revision_query.text.empty())}
        .With(Spacing(6.0F), Padding(EdgeInsets::Symmetric(16.0F, 5.0F)), CrossAlign(CrossAxisAlignment::Center))
        : View{},
    Divider(),
    columns,
    Divider(),
    tab->graph.empty()
        ? View(Column {
            SingleLine(tab->history_busy ? "Loading commits…" : "No commits on this history", kMuted),
          }.With(Grow(), Spacing(10.0F), MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center)))
        : VirtualList(tab->graph.size(), [=](std::size_t index) {
            const auto& commit = tab->graph[index].commit;
            const auto subject = commit.subject.empty() ? "(no commit message)" : commit.subject;
            const bool selected = tab->comparison.kind == git::DiffKind::Commit && tab->comparison.commit == commit.id;
            View refs;
            if (auto labels = tab->decorations.find(commit.id.hex);
                labels != tab->decorations.end() && !labels->second.empty()) {
              const auto count = labels->second.size();
              refs = Row {
                SingleLine(labels->second.front(), kAccent, 10.0F, FontWeight::SemiBold)
                    .With(Frame{.max_width = compact ? 54.0F : 85.0F}, Padding(EdgeInsets::Symmetric(5.0F, 3.0F)),
                          Background(kSelection), CornerRadius(4.0F)),
                count > 1 && detailed ? CodeLabel("+" + std::to_string(count - 1)) : View{},
              }.With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Center));
            }
            return Row {
              Spacer().With(Frame{.width = 3.0F, .height = kHistoryRowHeight}, Grow(0.0F),
                            Background(selected ? kAccent : Color::Transparent())),
              GraphGraphic(tab, index, graph_column - 3.0F, compact, selected),
              Row {
                refs,
                SingleLine(subject, kText, 12.0F, selected ? FontWeight::SemiBold : FontWeight::Regular).With(Grow()),
              }.With(Grow(), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)),
              detailed ? SingleLine(commit.author.name, kMuted, 12.0F).With(Frame{.width = author_column}) : View{},
              detailed ? SingleLine(CommitDate(commit.committed_at, false), kMuted, 11.5F)
                             .With(Frame{.width = date_column}) : View{},
              CodeLabel(commit.id.hex.substr(0, 8), kMuted, 11.0F).With(Frame{.width = hash_column}),
            }.With(Frame{.height = kHistoryRowHeight}, Padding(EdgeInsets{.right = 10.0F}),
                   CrossAlign(CrossAxisAlignment::Center), HoverFeedback(), Focusable(true),
                   Background(selected ? kSelection : index % 2 ? kWindow : kSurface),
                   Semantics{.role = SemanticRole::Button, .label = subject, .selected = selected})
                .OnClick([=, id = commit.id] {
                  tab->show_inspector = compact;
                  tasks.Launch(BrowseCommit(tab, id, revision));
                }).Key(commit.id.hex);
          }).ItemExtent(kHistoryRowHeight).Controller(scroll).With(Grow(), ScrollBar(), list_focus, Focusable(true))
              .On<ViewEvents::KeyIntercept>(move).Key("history-list"),
    Divider(),
    Row {
      CodeLabel(std::to_string(tab->graph.size()) + " commits loaded").With(Grow()),
      !tab->history_finished ? ReviewButton(tab->history_busy ? "Loading…" : "Load more",
          [=] { tasks.Launch(ReadHistory(tab, revision, !tab->history)); }, !tab->history_busy) : View{},
    }.With(Padding(EdgeInsets::Symmetric(14.0F, 4.0F)), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch), Background(kSurface));
}

std::size_t ChangedPaths(const Tab& tab) {
  if (!tab.status) return 0;
  return std::ranges::count_if(tab.status->files, [](const git::FileStatus& file) {
    return file.staged || file.unstaged;
  });
}

std::string FileName(std::string_view path) {
  const auto slash = path.rfind('/');
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::vector<std::string> StagePaths(const std::vector<git::FileStatus>& files) {
  std::vector<std::string> paths;
  for (const auto& file : files) {
    if (file.conflicted) continue;
    paths.push_back(file.path);
    if (!file.previous_path.empty() && file.previous_path != file.path) paths.push_back(file.previous_path);
  }
  std::ranges::sort(paths);
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

[[huxerui::composable]]
View WorkingChanges(std::shared_ptr<Tab> tab, TaskScope tasks, Revision revision, bool staged, bool compact) {
  static_cast<void>(static_cast<std::uint64_t>(revision));
  const auto scroll = UseScrollController();
  const auto list_focus = UseViewHandle();
  std::vector<git::FileStatus> files;
  if (tab->status) {
    for (const auto& file : tab->status->files) {
      if (!file.conflicted && (staged ? file.staged : file.unstaged)) files.push_back(file);
    }
  }
  const auto kind = staged ? git::DiffKind::Staged : git::DiffKind::Worktree;
  const bool can_stage = !tab->busy && !tab->operation;
  constexpr float heading_height = 34.0F;
  constexpr float bottom_inset = 8.0F;
  const float group_height = heading_height + bottom_inset +
      kChangeFileRowHeight * static_cast<float>(std::clamp<std::size_t>(files.size(), 1, 6));
  const auto select = [=](const std::string& path) {
    tab->select_worktree_file = false;
    tab->show_inspector = compact;
    tab->commit.reset();
    tab->comparison_files = {};
    tasks.Launch(ReadPatch(tab, {.kind = kind}, path, revision));
  };
  const auto update = [=](std::vector<std::string> paths) {
    auto edit = staged ? tab->Repo().UnstageFilesAsync(std::move(paths)) : tab->Repo().StageFilesAsync(std::move(paths));
    tasks.Launch(ChangeIndex(tab, revision, std::move(edit)));
  };
  const auto step = [=](const KeyEvent& event) {
    if (event.type != KeyEventType::Down || event.modifiers != KeyModifiers{} || files.empty() ||
        (event.key != Key::ArrowUp && event.key != Key::ArrowDown)) return false;
    const auto current = tab->comparison.kind == kind
        ? std::ranges::find(files, tab->selected_path, &git::FileStatus::path) : files.end();
    auto index = current == files.end() ? 0 : static_cast<std::size_t>(current - files.begin());
    if (current != files.end() && event.key == Key::ArrowDown) index = std::min(index + 1, files.size() - 1);
    else if (index) --index;
    const auto metrics = scroll.Metrics();
    const auto start = static_cast<float>(index) * kChangeFileRowHeight;
    if (start < metrics.offset) scroll.ScrollToItem(index, ScrollAlignment::Start);
    else if (start + kChangeFileRowHeight > metrics.offset + metrics.viewport_extent) {
      scroll.ScrollToItem(index, ScrollAlignment::End);
    }
    list_focus.RequestFocus(true);
    select(files[index].path);
    return true;
  };
  return Column {
    Row {
      SingleLine(staged ? "Staged changes" : "Unstaged changes", kText, 12.0F, FontWeight::SemiBold)
          .With(LayoutPriority(1.0F)),
      Badge(std::to_string(files.size()), kMuted, kElevated),
      Spacer(),
      ReviewButton(staged ? "Unstage all" : "Stage all",
                   [update, paths = StagePaths(files)] { update(paths); }, can_stage && !files.empty()),
    }.With(Frame{.height = heading_height}, Spacing(5.0F), Padding(EdgeInsets::Symmetric(12.0F, 0.0F)),
           CrossAlign(CrossAxisAlignment::Center)),
    files.empty()
        ? Text(staged ? "The index has no changes." : "The working tree has no unstaged changes.")
            .With(Padding(EdgeInsets::Symmetric(12.0F, 5.0F)), Grow(), Foreground(kMuted), FontSize(11.5F))
        : VirtualList(files, [=](const git::FileStatus& file) {
            const auto path = file.path;
            const auto name = FileName(path);
            const auto badge = file.deleted ? "D" : file.untracked ? "?" :
                               !file.previous_path.empty() && file.previous_path != file.path ? "R" : "M";
            const auto color = file.deleted ? kNegative : file.untracked ? kAccent : kWarning;
            const auto marker = file.deleted ? kRemoved : file.untracked ? kSelection : kWarningSurface;
            const bool selected = tab->selected_path == path && tab->comparison.kind == kind;
            return Row {
              Row {
                FileFlag(badge, color, marker),
                Column {
                  SingleLine(name, selected ? kAccent : kText, 12.0F,
                             selected ? FontWeight::SemiBold : FontWeight::Regular),
                  name != path ? SingleLine(path, kMuted, 10.5F) : View{},
                }.With(Grow(), Spacing(1.0F), CrossAlign(CrossAxisAlignment::Stretch)),
              }.With(Grow(), Frame{.height = kChangeFileRowHeight}, Spacing(7.0F),
                     Padding(EdgeInsets::Symmetric(8.0F, 0.0F)),
                     CrossAlign(CrossAxisAlignment::Center), HoverFeedback(), Focusable(true),
                     Semantics{.role = SemanticRole::Button, .label = path, .selected = selected})
                  .OnClick([=] { select(path); }),
              ReviewButton(staged ? "Unstage" : "Stage", [=] { update(StagePaths({file})); }, can_stage),
            }.With(Frame{.height = kChangeFileRowHeight}, Background(selected ? kSelection : kSurface),
                   CornerRadius(5.0F), Padding(EdgeInsets{.right = 5.0F}), Spacing(4.0F),
                   CrossAlign(CrossAxisAlignment::Center)).Key(path);
          }).ItemExtent(kChangeFileRowHeight).Controller(scroll)
              .With(Grow(), Padding(EdgeInsets::Symmetric(7.0F, 0.0F)), ScrollBar(), list_focus, Focusable(true))
              .On<ViewEvents::KeyIntercept>(step).Key(staged ? "staged-files" : "unstaged-files"),
    Spacer().With(Frame{.height = bottom_inset}, Grow(0.0F)),
  }.With(Grow(compact ? 1.0F : 0.0F), Frame{.height = compact ? std::nullopt : std::optional{group_height}},
         CrossAlign(CrossAxisAlignment::Stretch), Background(kSurface));
}

Task<void> BrowseBranch(std::shared_ptr<Tab> tab, std::string ref, Revision revision) {
  const auto generation = ++tab->review_generation;
  tab->page = WorkspacePage::History;
  tab->branch = std::move(ref);
  tab->anchor.clear();
  tab->show_inspector = false;
  tab->reading_review = false;
  tab->commit.reset();
  tab->comparison_files = {};
  tab->patch.reset();
  tab->selected_path.clear();
  co_await ReadHistory(tab, revision, true);
  if (generation == tab->review_generation && !tab->graph.empty()) {
    co_await BrowseCommit(tab, tab->graph.front().commit.id, revision);
  }
}

/// Supplies one dismissal path for the window shortcuts to yield to an active dialog.
DialogOptions WorkspaceDialogOptions(std::shared_ptr<Workspace> workspace, DialogHandle dialogs,
                                      std::shared_ptr<LayerId> layer) {
  workspace->modal_open = true;
  return {
    .dismiss_on_outside_press = false,
    .on_dismiss_request = [=] {
      workspace->modal_open = false;
      dialogs.Dismiss(*layer);
    },
  };
}

void ConfirmWorkspace(std::shared_ptr<Workspace> workspace, DialogHandle dialogs, std::string title,
                      std::string description, std::string accept, std::string cancel,
                      std::function<void()> confirmed) {
  const auto layer = std::make_shared<LayerId>();
  auto options = WorkspaceDialogOptions(workspace, dialogs, layer);
  *layer = dialogs.Show(std::move(title), std::move(description), std::move(accept), std::move(cancel),
      [workspace, confirmed = std::move(confirmed)] {
        workspace->modal_open = false;
        if (confirmed) confirmed();
      }, [workspace] { workspace->modal_open = false; }, std::move(options));
}

[[huxerui::composable]]
View BranchNameDialog(DialogContext dialog, std::shared_ptr<Workspace> workspace, std::string title, std::string details, std::string initial,
                      std::string submit, std::function<void(std::string, bool)> accept, bool offer_switch) {
  const auto input = UseState(TextEditingValue::FromText(initial));
  const TextEditingValue value = input;
  const auto finish = [=](bool switch_now) {
    const TextEditingValue name = input;
    if (name.text.empty()) return;
    workspace->modal_open = false;
    dialog.Dismiss();
    accept(name.text, switch_now);
  };
  return Column {
    Text(title).Style(TextStyle{Font::System(19.0F).WithWeight(FontWeight::SemiBold), kText}),
    Text(details).With(Foreground(kMuted)),
    TextField(value).Label("Branch name").OnChanged([input](const TextEditingValue& next) { input = next; })
        .OnSubmitted([=] { finish(false); }).Key("branch-name"),
    Flow {
      Button("Cancel").OnClick([dialog, workspace] { workspace->modal_open = false; dialog.Dismiss(); }),
      offer_switch ? Button("Create and switch").With(Enabled(!value.text.empty()))
          .OnClick([=] { finish(true); }) : View{},
      PrimaryAction(submit, [=] { finish(false); }, !value.text.empty()),
    }.With(Spacing(6.0F)),
  }.With(Frame{.width = 410.0F}, Padding(18.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

void EditBranchName(std::shared_ptr<Workspace> workspace, DialogHandle dialogs, std::string title,
                    std::string details, std::string initial, std::string submit,
                    std::function<void(std::string, bool)> accept, bool offer_switch) {
  const auto layer = std::make_shared<LayerId>();
  auto options = WorkspaceDialogOptions(workspace, dialogs, layer);
  *layer = dialogs.Show([=](DialogContext context) {
    return BranchNameDialog(context, workspace, title, details, initial, submit, accept, offer_switch);
  }, std::move(options));
}

Task<void> CreateLocalBranch(std::shared_ptr<Tab> tab, git::BranchInfo source, git::HeadState head,
                             std::string name, bool track, bool switch_now, Revision revision) {
  tab->busy = true;
  Changed(revision);
  const auto created = co_await tab->Repo().CreateBranchAsync(name, source.target);
  tab->issue = OutcomeText(created.report, "Branch created");
  if (created.report.Succeeded()) {
    bool ready = true;
    if (track) {
      const auto tracking = co_await tab->Repo().SetUpstreamAsync(name, ShortRef(source.name));
      tab->issue += " · " + OutcomeText(tracking.report, "Upstream configured");
      ready = tracking.report.Succeeded();
    }
    if (ready && switch_now) {
      const auto switched = co_await tab->Repo().SwitchAsync("refs/heads/" + name, head);
      tab->issue += " · " + OutcomeText(switched.report, "Branch switched");
    }
  }
  tab->busy = false;
  co_await Refresh(tab, revision);
  if (created.report.Succeeded()) co_await ReadHistory(tab, revision, true);
}

Task<void> ConfirmBranchDeletion(std::shared_ptr<Workspace> workspace, std::shared_ptr<Tab> tab,
                                git::BranchInfo branch, DialogHandle dialogs, TaskScope tasks, Revision revision) {
  const auto head = tab->info->head;
  std::string message = branch.name + "\nTip " + branch.target.hex;
  if (head.target) {
    const auto compared = co_await tab->Repo().CompareReachabilityAsync(branch.target, *head.target);
    if (!compared.Succeeded() || compared.Value().shallow) {
      message += "\nReachability could not be verified for the full history.";
    } else if (compared.Value().ahead) {
      message += "\n" + std::to_string(compared.Value().ahead) + " commit(s) absent from " + HeadLabel(head) + ".";
    }
  }
  if (CurrentTab(*workspace) != tab || workspace->adding_repository || tab->busy || tab->operation) co_return;
  ConfirmWorkspace(workspace, dialogs, "Delete local branch?", message, "Delete branch", "Cancel", [=] {
    if (CurrentTab(*workspace) == tab && !tab->busy && !tab->operation) {
      tasks.Launch(Change(tab, revision, tab->Repo().DeleteBranchAsync(branch.name, branch.target), "Branch deleted", true));
    }
  });
}

Task<void> IntegrateBranch(std::shared_ptr<Workspace> workspace, std::shared_ptr<Tab> tab, git::BranchInfo branch,
                           git::HeadState head, bool rebase, Revision revision) {
  if (CurrentTab(*workspace) != tab || workspace->adding_repository || tab->busy || tab->operation) co_return;
  tab->busy = true;
  Changed(revision);
  const auto refs = co_await tab->Repo().ListBranchesAsync();
  if (!refs.Succeeded() ||
      std::ranges::none_of(refs.Value(), [&](const git::BranchInfo& current) {
        return current.name == branch.name && current.target == branch.target;
      })) {
    tab->issue = refs.Succeeded() ? "The source branch changed. Review its new tip before integrating."
                                 : refs.Error().message;
    tab->busy = false;
    co_await Refresh(tab, revision);
    co_return;
  }
  auto operation = rebase ? tab->Repo().RebaseAsync(branch.target, head) : tab->Repo().MergeAsync(branch.target, head);
  co_await Integrate(tab, revision, std::move(operation));
}

std::vector<MenuEntry> BranchActions(std::shared_ptr<Workspace> workspace, std::shared_ptr<Tab> tab,
                                     git::BranchInfo branch, DialogHandle dialogs, TaskScope tasks, Revision revision,
                                     std::shared_ptr<huxerui::Clipboard> clipboard, std::function<void()> close) {
  const auto item = [workspace](StringVariant label, std::function<void()> run) {
    return MenuItem(std::move(label), [workspace, run = std::move(run)] {
      workspace->menu_open = false;
      run();
    });
  };
  const auto head = tab->info->head;
  const auto current = [=] {
    return CurrentTab(*workspace) == tab && !workspace->adding_repository;
  };
  const auto can_write = [=] {
    return current() && !tab->busy && !tab->operation;
  };
  const bool writable = can_write();
  const bool worktree = tab->info->work_directory.has_value();
  const auto display = ShortRef(branch.name);
  const auto create = [=](bool track) {
    if (!can_write()) return;
    const auto accept = [=](std::string name, bool switch_now) {
      if (can_write()) tasks.Launch(CreateLocalBranch(tab, branch, head, std::move(name), track, switch_now, revision));
    };
    std::string initial = display;
    if (track && initial.find('/') != std::string::npos) initial.erase(0, initial.find('/') + 1);
    EditBranchName(workspace, dialogs, track ? "Create tracking branch" : "Create local branch",
                 "Start at " + display + " · " + branch.target.hex.substr(0, 8),
                 track ? std::move(initial) : std::string{}, "Create", accept, worktree);
  };
  const auto integrate = [=](bool rebase) {
    if (!can_write()) return;
    const auto destination = HeadLabel(head) + " · " + head.target->hex.substr(0, 8);
    const auto source = display + " · " + branch.target.hex.substr(0, 8);
    ConfirmWorkspace(workspace, dialogs, rebase ? "Rebase " + HeadLabel(head) + "?" : "Merge into " + HeadLabel(head) + "?",
                 (rebase ? "Branch  " + destination + "\nNew base  " + source + "\nLinear rebase"
                         : "Source  " + source + "\nDestination  " + destination),
                 rebase ? "Start rebase" : "Merge", "Cancel", [=] {
      tasks.Launch(IntegrateBranch(workspace, tab, branch, head, rebase, revision));
    });
  };
  const bool can_integrate = writable && worktree && head.kind == git::HeadKind::Attached &&
                             head.target && *head.target != branch.target;
  std::vector<MenuEntry> entries{
    item(display, [] {}).Enabled(false),
    MenuSection{},
    item("View history", [=] {
      if (!current()) return;
      if (close) close();
      tasks.Launch(BrowseBranch(tab, branch.name, revision));
    }),
    item("Copy ref name", [=] {
      if (!current()) return;
      tab->issue = clipboard->WriteText(branch.name) ? "Reference copied" : "Clipboard unavailable";
      Changed(revision);
    }),
    MenuSection{},
    item(branch.remote ? "Create tracking branch…" : "Switch to branch", [=] {
      if (!can_write()) return;
      if (branch.remote) {
        create(true);
      } else {
        tasks.Launch(Change(tab, revision, tab->Repo().SwitchAsync(branch.name, head), "Branch switched", true));
      }
    }).Enabled(writable && (branch.remote || (worktree && !branch.checked_out && !branch.is_head))),
    item("Create branch here…", [=] { create(false); }).Enabled(writable),
    item("Merge into " + HeadLabel(head) + "…", [=] { integrate(false); }).Enabled(can_integrate),
    item("Rebase " + HeadLabel(head) + " onto this branch…", [=] { integrate(true); }).Enabled(can_integrate),
  };
  if (!branch.remote) {
    entries.emplace_back(MenuSection{});
    entries.emplace_back(item("Rename…", [=] {
      if (!can_write()) return;
      const auto accept = [=](std::string name, bool) {
        if (can_write()) tasks.Launch(Change(tab, revision, tab->Repo().RenameBranchAsync(branch.name, name, branch.target),
                                            "Branch renamed", true));
      };
      EditBranchName(workspace, dialogs, "Rename branch", branch.name, display, "Rename", accept, false);
    }).Enabled(writable && (!branch.checked_out || branch.is_head)));
    std::vector<MenuEntry> upstreams;
    const auto set_upstream = [=](std::optional<std::string> ref) {
      if (!can_write()) return;
      const auto target = ref.value_or("No upstream");
      ConfirmWorkspace(workspace, dialogs, "Change upstream?", branch.name + "\nCurrent  " +
                   (branch.upstream ? ShortRef(*branch.upstream) : "No upstream") + "\nNew  " + target,
                   "Save upstream", "Cancel", [=] {
        if (can_write()) tasks.Launch(Change(tab, revision, tab->Repo().SetUpstreamAsync(branch.name, ref), "Upstream updated"));
      });
    };
    for (const auto& remote_branch : tab->branches) {
      if (remote_branch.remote) {
        const auto short_ref = ShortRef(remote_branch.name);
        upstreams.emplace_back(item(short_ref, [=] { set_upstream(short_ref); }).Checked(branch.upstream == remote_branch.name));
      }
    }
    upstreams.emplace_back(item("Clear upstream", [=] { set_upstream({}); }).Enabled(branch.upstream.has_value()));
    entries.emplace_back(MenuItem("Set upstream", std::move(upstreams)).Enabled(writable));
    entries.emplace_back(item("Delete local branch…", [=] {
      if (can_write()) tasks.Launch(ConfirmBranchDeletion(workspace, tab, branch, dialogs, tasks, revision));
    }).Enabled(writable && !branch.checked_out && !branch.is_head));
  }
  return entries;
}

[[huxerui::composable]]
View BranchRow(std::shared_ptr<Workspace> workspace, std::shared_ptr<Tab> tab, git::BranchInfo branch,
               DialogHandle dialogs, TaskScope tasks, Revision revision,
               std::shared_ptr<huxerui::Clipboard> clipboard, std::function<void()> close) {
  const auto anchor = UseViewHandle();
  const auto menus = UseMenu();
  const auto entries = [=] { return BranchActions(workspace, tab, branch, dialogs, tasks, revision, clipboard, close); };
  const auto show = [=](std::optional<Point> position) {
    const auto layer = std::make_shared<LayerId>();
    workspace->menu_open = true;
    MenuOptions options{.on_dismiss_request = [=] {
      workspace->menu_open = false;
      menus.Dismiss(*layer);
    }};
    *layer = position ? menus.ShowAt(*position, entries(), options) : menus.Show(anchor, entries(), options);
  };
  const auto select = [=] {
    if (close) close();
    tasks.Launch(BrowseBranch(tab, branch.name, revision));
  };
  return Row {
    SelectableRow(ShortRef(branch.name), tab->branch == branch.name, select, branch.is_head ? Badge("HEAD") : View{},
                  Glyph(WorkspaceIcon::Branch, branch.is_head ? kAccent : kMuted))
        .With(Grow()).Key(branch.name),
    SingleLine("⋯", kMuted, 17.0F)
        .With(Frame{.width = 26.0F, .height = 26.0F}, Padding(EdgeInsets::Symmetric(5.0F, 0.0F)),
              CornerRadius(5.0F), HoverFeedback(), Focusable(true), Tooltip("Actions for " + ShortRef(branch.name)),
              Semantics{.role = SemanticRole::Button, .label = "Actions for " + ShortRef(branch.name)}, anchor)
        .OnClick([=] { show({}); }).Key("actions:" + branch.name),
  }.With(CrossAlign(CrossAxisAlignment::Center), Spacing(2.0F))
      .On<ViewEvents::ContextMenuRequested>([=](Point position) { show(position); });
}

View Sidebar(std::shared_ptr<Workspace> workspace, std::shared_ptr<Tab> tab, std::shared_ptr<git::GitService> service,
             TaskScope tasks, TaskScope app_tasks, Revision revision, DialogHandle dialogs,
             std::shared_ptr<huxerui::Clipboard> clipboard, std::function<void()> close) {
  const auto show_page = [=](WorkspacePage page) {
    SelectPage(tab, page, tasks, revision);
    if (close) close();
  };
  std::vector<View> rows{
      SelectableRow("History", tab->page == WorkspacePage::History, [=] { show_page(WorkspacePage::History); },
                    {}, Glyph(WorkspaceIcon::History))
          .Key("workspace-history"),
  };
  if (tab->info && tab->info->work_directory) {
    rows.push_back(SelectableRow("Changes", tab->page == WorkspacePage::Changes,
                                 [=] { show_page(WorkspacePage::Changes); },
                                 Badge(std::to_string(ChangedPaths(*tab)), kMuted, kElevated),
                                 Glyph(WorkspaceIcon::Changes))
        .Key("workspace-changes"));
  }
  if (tab->operation) {
    rows.push_back(SelectableRow("Conflicts", tab->page == WorkspacePage::Conflicts,
                                 [=] { show_page(WorkspacePage::Conflicts); },
                                 Badge(std::to_string(tab->operation->conflicts.size()), kWarning, kWarningSurface),
                                 Glyph(WorkspaceIcon::Conflict, kWarning)));
  }

  const auto add_branches = [&](bool remote) {
    const auto count = std::ranges::count(tab->branches, remote, &git::BranchInfo::remote);
    if (remote && count == 0) return;
    rows.push_back(Divider());
    rows.push_back(LabelSection(remote ? "REMOTE BRANCHES" : "LOCAL BRANCHES", count));
    for (const auto& branch : tab->branches) {
      if (branch.remote != remote) continue;
      rows.push_back(BranchRow(workspace, tab, branch, dialogs, tasks, revision, clipboard, close)
          .Key("branch:" + branch.name));
    }
  };
  add_branches(false);
  add_branches(true);

  if (!tab->tags.empty()) {
    rows.push_back(Divider());
    rows.push_back(LabelSection("TAGS", tab->tags.size()));
    for (const auto& tag : tab->tags) {
      const auto id = tag.peeled ? tag.peeled : tag.target;
      if (id) rows.push_back(SelectableRow(ShortRef(tag.name), false, [=] {
        show_page(WorkspacePage::History);
        tab->show_inspector = static_cast<bool>(close);
        tasks.Launch(BrowseCommit(tab, *id, revision));
      }).Key(tag.name));
    }
  }
  if (!tab->remotes.empty()) {
    rows.push_back(Divider());
    rows.push_back(LabelSection("REMOTES", tab->remotes.size()));
    for (const auto& remote : tab->remotes) {
      rows.push_back(SelectableRow(remote.name, tab->remote == remote.name, [=] {
        tab->remote = remote.name;
        Changed(revision);
      }).Key(remote.name));
    }
  }
  if (tab->info && tab->info->work_directory) {
    rows.push_back(Divider());
    rows.push_back(LabelSection("STASHES", tab->stashes.size()));
    rows.push_back(SelectableRow("Stash tracked changes", false, [=] {
      if (!tab->busy && !tab->operation) {
        tasks.Launch(Change(tab, revision, tab->Repo().SaveStashAsync({}), "Stash saved"));
      }
    }));
    for (const auto& stash : tab->stashes) {
      rows.push_back(Row {
        SelectableRow(stash.message, false, [=] {
          show_page(WorkspacePage::History);
          tab->show_inspector = static_cast<bool>(close);
          tasks.Launch(BrowseCommit(tab, stash.id, revision));
        }).With(Grow()),
        Button("Apply").With(Enabled(!tab->busy && !tab->operation)).OnClick([=] {
          tasks.Launch(Change(tab, revision, tab->Repo().ApplyStashAsync(stash), "Stash applied"));
        }),
      }.With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Center)).Key(stash.id.hex));
    }
  }
  if (!tab->submodules.empty()) {
    rows.push_back(Divider());
    rows.push_back(LabelSection("SUBMODULES", tab->submodules.size()));
    for (const auto& item : tab->submodules) {
      rows.push_back(Row {
        SingleLine(item.path).With(Grow()),
        Button("Update").With(Enabled(!workspace->transfer && item.recorded.has_value())).OnClick([=] {
          StartTransfer(workspace, tab, service->StartUpdateSubmodules(tab->Repo(), {item.name}),
                        "Update submodule", app_tasks, revision);
        }),
      }.With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Center)).Key(item.path));
    }
  }
  if (!tab->worktrees.empty()) {
    rows.push_back(Divider());
    rows.push_back(LabelSection("WORKTREES", tab->worktrees.size()));
    for (const auto& item : tab->worktrees) {
      rows.push_back(SelectableRow(item.name, false, [=] {
        if (item.valid) {
          if (close) close();
          tasks.Launch(OpenDirectory(workspace, service, item.directory, revision));
        }
      }).With(Enabled(item.valid)).Key(item.name));
    }
  }
  View settings;
  if (tab->edit_repository) {
    const auto target = tab->info ? tab->info->head.target : std::nullopt;
    settings = Column {
      TextField(tab->new_branch).Label("New branch").OnChanged([=](const TextEditingValue& value) {
        tab->new_branch = value;
        Changed(revision);
      }),
      Button("Create at HEAD").With(Enabled(target && !tab->new_branch.text.empty() && !tab->busy)).OnClick([=] {
        tasks.Launch(Change(tab, revision, tab->Repo().CreateBranchAsync(tab->new_branch.text, *target),
                            "Branch created"));
      }),
      TextField(tab->new_remote_name).Label("Remote name").OnChanged([=](const TextEditingValue& value) {
        tab->new_remote_name = value;
        Changed(revision);
      }),
      TextField(tab->new_remote_url).Label("Remote URL").OnChanged([=](const TextEditingValue& value) {
        tab->new_remote_url = value;
        Changed(revision);
      }),
      Button("Add remote").With(Enabled(!tab->new_remote_name.text.empty() && !tab->new_remote_url.text.empty() &&
                                        !tab->busy)).OnClick([=] {
        tasks.Launch(Change(tab, revision, tab->Repo().AddRemoteAsync(tab->new_remote_name.text, tab->new_remote_url.text),
                            "Remote added"));
      }),
    }.With(Spacing(6.0F), Padding(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
  }
  return Column {
    ScrollView(Column(rows).With(Spacing(3.0F), Padding(8.0F), CrossAlign(CrossAxisAlignment::Stretch)))
        .With(ScrollBar(), Grow()),
    Divider(),
    settings,
    SelectableRow(tab->edit_repository ? "Hide repository settings" : "Repository settings",
                  tab->edit_repository, [=] {
      tab->edit_repository = !tab->edit_repository;
      Changed(revision);
    }).With(Padding(EdgeInsets::Symmetric(8.0F, 5.0F))),
  }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch), Background(kSidebar));
}

constexpr float kDiffRowHeight = 23.0F;
constexpr float kHunkRowHeight = 26.0F;

View DiffCell(std::shared_ptr<Tab> tab, Revision revision, const git::DiffLine* line, bool left,
              bool stageable, float side_width = 0.0F, float offset = 0.0F) {
  const bool split = side_width > 0.0F;
  const std::optional<float> width = split ? std::optional{side_width} : std::nullopt;
  const Frame frame{.width = width, .height = kDiffRowHeight, .min_width = width, .max_width = width};
  if (!line) return Row{}.With(frame, Grow(0.0F), Background(kToolbar));
  const bool added = line->kind == git::LineKind::Added;
  const bool removed = line->kind == git::LineKind::Deleted;
  const bool selected = std::ranges::find(tab->selected_lines, line->id) != tab->selected_lines.end();
  const auto tint = selected ? Color::Rgb(22, 62, 156) : added ? kPositive : removed ? kNegative : kText;
  const auto fill = selected ? kSelectedLine : added ? kAdded : removed ? kRemoved : kSurface;
  const auto number = [](int value) { return value >= 0 ? std::to_string(value) : std::string{}; };
  const auto toggle = [tab, revision, id = line->id] {
    if (std::erase(tab->selected_lines, id) == 0) tab->selected_lines.push_back(id);
    Changed(revision);
  };
  const auto line_number = [number](int value, float extent) {
    return Text(number(value)).Style(TextStyle{Font::Monospace(10.0F), kMuted}).LineLimit(1)
        .Align(TextAlign::Trailing).With(Frame{.width = extent}, Padding(EdgeInsets{.right = 7.0F}));
  };
  const auto text = DisplayLine(line->text);
  const TextStyle style{Font::Monospace(11.5F), tint};
  auto code = split ? View(Canvas([=](PaintContext& paint, Size size) {
    const Rect clip{0.0F, 0.0F, size.width, size.height};
    paint.PushClip(clip);
    paint.DrawText({-offset, 0.0F, size.width + offset, size.height}, text, style,
                   {.vertical_align = TextVerticalAlign::Center, .wrap = TextWrap::NoWrap});
    paint.PopClip();
  }).With(Semantics{.role = SemanticRole::Text, .label = text})) :
      View(Text(text).Style(style).LineLimit(1).Overflow(TextOverflow::Clip));
  auto content = Row {
    Spacer().With(Frame{.width = 3.0F, .height = kDiffRowHeight}, Grow(0.0F),
                  Background(selected ? kAccent : Color::Transparent())),
    line_number(left || !split ? line->old_line : line->new_line, split ? 30.0F : 28.0F),
    !split ? line_number(line->new_line, 31.0F) : View{},
    CodeLabel(added ? "+" : removed ? "−" : "", tint, 11.5F).Align(TextAlign::Center)
        .With(Frame{.width = split ? 18.0F : 23.0F}),
    std::move(code).With(Grow(), Frame{.height = kDiffRowHeight}),
  }.With(frame, CrossAlign(CrossAxisAlignment::Center), Background(fill));
  if (stageable && (added || removed)) {
    const auto number_label = removed ? line->old_line : line->new_line;
    return std::move(content).OnClick(toggle).With(HoverFeedback(), Focusable(true),
        Semantics{.role = SemanticRole::Button, .label = std::string(added ? "Added line " : "Deleted line ") +
            std::to_string(number_label), .selected = selected});
  }
  return content;
}

View InspectorPlaceholder(std::string title, std::string detail) {
  return Column {
    Text(std::move(title)).Style(TextStyle{Font::System(16.0F).WithWeight(FontWeight::SemiBold), kText}),
    Text(std::move(detail)).With(Foreground(kMuted), FontSize(13.0F)),
  }.With(Grow(), Spacing(8.0F), MainAlign(MainAxisAlignment::Center),
         CrossAlign(CrossAxisAlignment::Center), Padding(18.0F), Background(kSurface));
}

[[huxerui::composable]]
View PatchPane(std::shared_ptr<Tab> tab, TaskScope tasks, Revision revision, bool compact) {
  static_cast<void>(static_cast<std::uint64_t>(revision));
  const auto horizontal = UseScrollController();
  const auto review = tab->patch;
  if (!review) {
    if (tab->reading_review) return InspectorPlaceholder("Loading diff…", tab->selected_path);
    if (!tab->selected_path.empty()) return InspectorPlaceholder("Diff unavailable", tab->selected_path);
    return InspectorPlaceholder("No file selected", "Select a changed file to review its diff.");
  }
  const auto& patch = review->diff;
  if (patch.file.binary) return InspectorPlaceholder("Binary file", "Use file-level staging for this change.");
  if (patch.truncated) return InspectorPlaceholder("Diff limit reached", "This file exceeds the text review limit.");
  if (patch.hunks.empty()) return InspectorPlaceholder("No textual changes", "Review the file status for this change.");
  const bool staged = tab->comparison.kind == git::DiffKind::Staged;
  const bool stageable = patch.staging.IsValid();
  const bool split = tab->side_by_side && !compact;
  const auto& rows = split ? review->split : review->unified;
  const float columns = static_cast<float>(review->width) * 7.7F;
  const auto metrics = horizontal.Metrics();
  const float viewport_width = metrics.viewport_extent > 0.0F ? metrics.viewport_extent : 650.0F;
  const float side_width = std::max(1.0F, (viewport_width - 1.0F) / 2.0F);
  const float offset = metrics.offset;
  // In split view the gutters stay in place while both text columns move by the same amount.
  const float content_width = split ? viewport_width + std::max(0.0F, columns + 64.0F - side_width)
                                   : std::max(viewport_width, columns + 105.0F);
  auto list = VirtualList(rows.size(), [=](std::size_t index) -> View {
    const auto& row = (split ? review->split : review->unified)[index];
    const auto& hunk = review->diff.hunks[row.hunk];
    if (!row.left && !row.right) {
      auto header = Row {
        CodeLabel(DisplayLine(hunk.header), Color::Rgb(49, 84, 145), 11.0F).With(Grow(), LayoutPriority(0.0F)),
        stageable ? SingleLine(staged ? "Unstage hunk" : "Stage hunk", tab->busy ? kMuted : kAccent,
                               10.5F, FontWeight::SemiBold)
            .VerticalAlign(TextVerticalAlign::Center)
            .With(Frame{.height = 20.0F}, Padding(EdgeInsets::Symmetric(7.0F, 0.0F)),
                  Background(kSurface), CornerRadius(4.0F), Border{.color = kBorder}, LayoutPriority(1.0F),
                  HoverFeedback(), Focusable(true), Enabled(!tab->busy),
                  Semantics{.role = SemanticRole::Button})
            .OnClick([=, id = hunk.id] { tasks.Launch(ApplyEdits(tab, revision, review, id)); })
            .Key("hunk-action-" + std::to_string(hunk.id)) : View{},
      }.With(Frame{.width = viewport_width, .height = kHunkRowHeight, .max_width = viewport_width}, Grow(0.0F), Spacing(10.0F),
             Padding(EdgeInsets::Symmetric(10.0F, 0.0F)), CrossAlign(CrossAxisAlignment::Center),
             Background(kHunk), Offset(Point{offset, 0.0F}));
      // The virtual item spans the whole patch. Its inner header stays the width of the visible pane.
      return Stack {std::move(header)}.With(Frame{.height = kHunkRowHeight}, Background(kHunk),
                                         Align(HorizontalAlignment::Start, VerticalAlignment::Start))
          .Key("hunk-" + std::to_string(hunk.id));
    }
    if (split) {
      return Row {
        DiffCell(tab, revision, row.left, true, stageable, side_width, offset),
        Divider(Axis::Vertical),
        DiffCell(tab, revision, row.right, false, stageable, side_width, offset),
      }.With(Frame{.width = viewport_width, .height = kDiffRowHeight, .max_width = viewport_width},
             CrossAlign(CrossAxisAlignment::Stretch), Offset(Point{offset, 0.0F}));
    }
    return DiffCell(tab, revision, row.left, true, stageable);
  }).EstimatedItemExtent(kDiffRowHeight)
      .With(Frame{.min_width = content_width}, ScrollBar()).Key("diff-lines");
  auto diff_scroll = ScrollView(list).ScrollAxis(Axis::Horizontal).Controller(horizontal)
      .With(Grow(), ScrollBar())
      .Key(std::string(split ? "split:" : "unified:") + tab->selected_path);
  return Column {
    Row {
      Glyph(WorkspaceIcon::File),
      Text(tab->selected_path).Style(TextStyle{Font::Monospace(12.0F).WithWeight(FontWeight::SemiBold), kText})
          .LineLimit(1).Overflow(TextOverflow::MiddleEllipsis),
      tab->comparison.kind == git::DiffKind::Commit ? View{} :
          (staged ? Badge("Staged", kPositive, kAdded) : Badge("Unstaged", kWarning, kWarningSurface))
              .With(LayoutPriority(1.0F)),
      Spacer(),
      Badge(std::to_string(patch.hunks.size()) + (patch.hunks.size() == 1 ? " hunk" : " hunks"), kMuted, kElevated)
          .With(LayoutPriority(1.0F)),
      !compact ? SegmentedButton({StringVariant("Unified"), StringVariant("Split")}, split ? 1u : 0u)
          .OnChanged([=](std::size_t index) {
            horizontal.ScrollTo(0.0F);
            tab->side_by_side = index == 1;
            Changed(revision);
          }).With(LayoutPriority(1.0F)) : View{},
    }.With(Frame{.height = 41.0F}, Spacing(8.0F), Padding(EdgeInsets::Symmetric(11.0F, 0.0F)),
           CrossAlign(CrossAxisAlignment::Center),
           Background(kSurface)),
    Divider(),
    split ? Row {
      SingleLine("Before", kMuted, 11.0F, FontWeight::SemiBold)
          .With(Frame{.width = side_width}, Padding(EdgeInsets::Symmetric(10.0F, 4.0F))),
      Divider(Axis::Vertical).With(Frame{.height = 24.0F}),
      SingleLine("After", kMuted, 11.0F, FontWeight::SemiBold)
          .With(Frame{.width = side_width}, Padding(EdgeInsets::Symmetric(10.0F, 4.0F))),
    }.With(Frame{.height = 24.0F}, Background(kToolbar), CrossAlign(CrossAxisAlignment::Center)) : View{},
    diff_scroll,
    stageable ? Divider() : View{},
    stageable ? Row {
      SingleLine(tab->selected_lines.empty() ? "Select changed lines to apply individually."
                                            : std::to_string(tab->selected_lines.size()) + " selected lines",
                 kMuted, 11.0F).With(Grow()),
      ReviewButton(staged ? "Unstage selected" : "Stage selected",
                   [=] { tasks.Launch(ApplyEdits(tab, revision, review, std::nullopt)); },
                   !tab->busy && !tab->selected_lines.empty(), true),
    }.With(Spacing(7.0F), Padding(EdgeInsets::Symmetric(11.0F, 6.0F)), CrossAlign(CrossAxisAlignment::Center),
           Background(kToolbar)) : View{},
  }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch), Background(kSurface)).Key("patch-pane");
}

View CommitFiles(std::shared_ptr<Tab> tab, TaskScope tasks, Revision revision) {
  const auto& files = tab->comparison_files.files;
  constexpr float row_height = 29.0F;
  return Column {
    Row {
      SingleLine("Changed files", kText, 11.0F, FontWeight::SemiBold).With(Grow()),
      Badge(std::to_string(files.size()), kMuted, kElevated),
    }.With(Frame{.height = 31.0F}, Padding(EdgeInsets::Symmetric(13.0F, 0.0F)),
           CrossAlign(CrossAxisAlignment::Center), Background(kSidebar)),
    Divider(),
    files.empty()
        ? Text("No changed files").With(Foreground(kMuted), Padding(12.0F), FontSize(12.0F))
        : VirtualList(files, [=](const git::FileDelta& file) {
            const auto path = file.new_path.empty() ? file.old_path : file.new_path;
            const char* label = file.kind == git::DeltaKind::Added ? "A" : file.kind == git::DeltaKind::Deleted ? "D" :
                                file.kind == git::DeltaKind::Renamed ? "R" : "M";
            const auto tint = file.kind == git::DeltaKind::Added ? kPositive : file.kind == git::DeltaKind::Deleted
                                                                           ? kNegative : kWarning;
            const auto marker = file.kind == git::DeltaKind::Added ? kAdded : file.kind == git::DeltaKind::Deleted
                                                                           ? kRemoved : kWarningSurface;
            const bool selected = tab->selected_path == path;
            return Row {
              FileFlag(label, tint, marker),
              SingleLine(path, selected ? kAccent : kText, 11.5F,
                         selected ? FontWeight::SemiBold : FontWeight::Regular).With(Grow()),
            }.With(Frame{.height = row_height}, Spacing(7.0F), Padding(EdgeInsets::Symmetric(9.0F, 0.0F)),
                   CrossAlign(CrossAxisAlignment::Center), Background(selected ? kSelection : kSurface),
                   CornerRadius(5.0F), HoverFeedback(), Focusable(true),
                   Semantics{.role = SemanticRole::Button, .label = path, .selected = selected})
                .OnClick([=] { tasks.Launch(ReadPatch(tab, tab->comparison, path, revision)); }).Key(path);
          }).ItemExtent(row_height).With(Grow(), Padding(EdgeInsets::Symmetric(6.0F, 5.0F)),
                                         ScrollBar()).Key("commit-files"),
  }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch), Background(kSurface));
}

/// Takes up to two UTF-8 initials for the commit author badge.
/// @param name Author name from the commit.
/// @return The initials, or a placeholder when the name is empty.
std::string AuthorInitials(std::string_view name) {
  std::string initials;
  for (int word = 0; word < 2; ++word) {
    const auto first = name.find_first_not_of(" \t");
    if (first == std::string_view::npos) break;
    name.remove_prefix(first);
    std::size_t end = 1;
    while (end < name.size() && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) ++end;
    initials.append(name.substr(0, end));
    if (end == 1 && initials.back() >= 'a' && initials.back() <= 'z') {
      initials.back() = static_cast<char>(initials.back() - 'a' + 'A');
    }
    const auto whitespace = name.find_first_of(" \t", end);
    if (whitespace == std::string_view::npos) break;
    name.remove_prefix(whitespace);
  }
  return initials.empty() ? "?" : initials;
}

View CommitInspector(std::shared_ptr<Tab> tab, TaskScope tasks, Revision revision, bool compact,
                     const std::shared_ptr<huxerui::Clipboard>& clipboard) {
  if (!tab->commit) {
    return InspectorPlaceholder(tab->reading_review ? "Loading commit…" : "No commit selected",
                                tab->reading_review ? "" : "Select a commit in History to see its files.");
  }
  const auto& commit = *tab->commit;
  const auto selected = commit.summary.id;
  const auto mainline = commit.summary.parents.size() > 1
      ? static_cast<unsigned>(tab->comparison.parent_index + 1) : 0u;
  const bool can_integrate = !tab->busy && !tab->operation && tab->info && tab->info->work_directory;
  std::vector<View> parents;
  for (std::size_t i = 0; i < commit.summary.parents.size(); ++i) {
    const auto& id = commit.summary.parents[i];
    const bool active = i == tab->comparison.parent_index;
    auto button_style = ReviewButtonStyle(false, active);
    button_style.minimum_height = 22.0F;
    button_style.padding = EdgeInsets::Symmetric(6.0F, 2.0F);
    ThemeDefinition style;
    style.Set(button_style);
    parents.push_back(Theme(style, Button(id.hex.substr(0, 8))
        .With(Semantics{.label = "Compare with parent " + id.hex.substr(0, 8),
                        .selected = active},
              CornerRadius(5.0F), Border{.color = active ? Color::Rgb(193, 208, 241) : kBorder})
        .OnClick([=] { tasks.Launch(BrowseCommit(tab, selected, revision, i)); }).Key(id.hex + std::to_string(i))));
  }
  auto metadata = Column {
    Text(commit.summary.subject.empty() ? "(no commit message)" : commit.summary.subject)
        .Style(TextStyle{Font::System(17.0F).WithWeight(FontWeight::Bold), kText})
        .LineLimit(2).Overflow(TextOverflow::Ellipsis),
    Row {
      Row {SingleLine(AuthorInitials(commit.summary.author.name), kAccent, 10.0F, FontWeight::Bold)}
          .With(Frame{.width = 25.0F, .height = 25.0F}, MainAlign(MainAxisAlignment::Center),
                CrossAlign(CrossAxisAlignment::Center), Background(kSelection), CornerRadius(13.0F)),
      SingleLine(commit.summary.author.name, kText, 11.5F, FontWeight::SemiBold).With(Grow()),
      SingleLine(CommitDate(commit.summary.committed_at), kMuted, 11.0F),
      CodeLabel(selected.hex.substr(0, 8), kMuted, 10.5F).Key("commit-hash:" + selected.hex),
      Row {Glyph(WorkspaceIcon::Copy)}
          .With(Frame{.width = 22.0F, .height = 23.0F}, MainAlign(MainAxisAlignment::Center),
                CrossAlign(CrossAxisAlignment::Center), CornerRadius(4.0F), HoverFeedback(), Focusable(true),
                Semantics{.role = SemanticRole::Button, .label = "Copy full commit hash"})
          .OnClick([=] {
            tab->issue = clipboard->WriteText(selected.hex) ? "Commit hash copied" : "Clipboard unavailable";
            Changed(revision);
          }),
    }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Center)),
    commit.message == commit.summary.subject || commit.message == commit.summary.subject + "\n"
        ? View{} : ScrollView(
            Text(commit.message).With(Foreground(kMuted), FontSize(12.0F)))
            .With(Frame{.max_height = 66.0F}, ScrollBar()),
    parents.empty() ? View{} : Row {
      SingleLine(parents.size() > 1 ? "Compare parent" : "Parent", kMuted, 11.0F),
      ScrollView(Row(parents).With(Spacing(4.0F))).ScrollAxis(Axis::Horizontal).With(Grow()),
    }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)),
    Flow {
      ReviewButton("Cherry-pick", [=] {
        tasks.Launch(Integrate(tab, revision, tab->Repo().CherryPickAsync(selected, mainline)));
      }, can_integrate),
      ReviewButton("Revert", [=] {
        tasks.Launch(Integrate(tab, revision, tab->Repo().RevertAsync(selected, mainline)));
      }, can_integrate),
    }.With(Spacing(5.0F)),
  }.With(Spacing(8.0F), Padding(EdgeInsets{.top = 12.0F, .right = 13.0F, .bottom = 10.0F, .left = 13.0F}),
         CrossAlign(CrossAxisAlignment::Stretch));
  if (!compact) {
    return Row {
      Column {
        metadata,
        Divider(),
        CommitFiles(tab, tasks, revision),
      }.With(Frame{.width = 292.0F, .min_width = 230.0F}, CrossAlign(CrossAxisAlignment::Stretch)),
      Divider(Axis::Vertical),
      PatchPane(tab, tasks, revision, false),
    }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch), Background(kSurface));
  }
  if (!tab->selected_path.empty()) {
    return Column {
      Row {
        Button("Changed files").OnClick([=] {
          ++tab->review_generation;
          tab->reading_review = false;
          tab->patch.reset();
          tab->selected_path.clear();
          Changed(revision);
        }),
        CodeLabel(selected.hex.substr(0, 8)).With(Grow()),
      }.With(Spacing(8.0F), Padding(6.0F), Background(kToolbar), CrossAlign(CrossAxisAlignment::Center)),
      Divider(),
      PatchPane(tab, tasks, revision, compact),
    }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch));
  }
  return Column {
    metadata,
    Divider(),
    CommitFiles(tab, tasks, revision),
  }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch));
}

View ConflictInspector(std::shared_ptr<Tab> tab, TaskScope tasks, Revision revision) {
  if (!tab->operation) return InspectorPlaceholder("No operation in progress", "Select History or Changes to continue.");
  const auto operation = *tab->operation;
  const auto conflict = std::ranges::find(operation.conflicts, tab->resolved_conflict, &git::ConflictEntry::path);
  if (conflict == operation.conflicts.end()) {
    return InspectorPlaceholder(operation.conflicts.empty() ? "Ready to continue" : "No conflict selected",
        operation.conflicts.empty() ? "Continue the operation from the conflicts workspace."
                                    : "Select a conflicted file to inspect the incoming and current content.");
  }
  if (!tab->reviewed_conflict) {
    return InspectorPlaceholder("Loading conflict…", tab->resolved_conflict);
  }
  if (*tab->reviewed_conflict != operation.version) {
    return InspectorPlaceholder("Conflict changed", "Select the file again to load the latest conflict.");
  }
  const auto entry = *conflict;
  const auto resolve = [tab, tasks, revision, path = entry.path, version = operation.version](git::Resolution resolution) {
    tasks.Launch(Resolve(tab, revision, path, version, resolution));
  };
  const auto use_side = [=](git::Resolution resolution) {
    const bool ours = resolution == git::Resolution::Ours;
    if (!tab->can_edit_conflict || !(ours ? entry.ours : entry.theirs)) {
      resolve(resolution);
      return;
    }
    tab->editor = TextEditingValue::FromText(ours ? tab->ours_text : tab->theirs_text);
    Changed(revision);
  };
  const auto side = [=](std::string title, const std::string& content, std::optional<git::Resolution> resolution) -> View {
    return Column {
      Row {
        SingleLine(std::move(title), kText, 12.0F, FontWeight::SemiBold).With(Grow()),
        resolution ? Button(*resolution == git::Resolution::Ours ? "Use ours" : "Use theirs")
            .With(Enabled(!tab->busy)).OnClick([=] { use_side(*resolution); }) : View{},
      }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)),
      ScrollView(Text(content).Style(TextStyle{Font::Monospace(11.0F), kText}))
          .With(Frame{.height = 105.0F}, ScrollBar()),
    }.With(Grow(), Spacing(5.0F), Padding(8.0F), Background(kToolbar), CornerRadius(6.0F),
           CrossAlign(CrossAxisAlignment::Stretch));
  };
  return ScrollView(Column {
    Row {
      CodeLabel(entry.path, kText, 12.0F).With(Grow()),
      Button(tab->show_conflict_base ? "Hide base" : "View base").OnClick([=] {
        tab->show_conflict_base = !tab->show_conflict_base;
        Changed(revision);
      }),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
    Row {side("Current", tab->ours_text, git::Resolution::Ours),
         side("Incoming", tab->theirs_text, git::Resolution::Theirs)}
        .With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    tab->show_conflict_base ? side("Base", tab->base_text, {}).With(Grow(0.0F)) : View{},
    tab->can_edit_conflict
        ? TextField(tab->editor).Label("Result").LineLimits(TextFieldLineLimits::MultiLine(5, 10))
              .OnChanged([=](const TextEditingValue& value) { tab->editor = value; Changed(revision); })
        : Text("Choose a side or stage an external edit for this file.").With(Foreground(kMuted)),
    Flow {
      Button("Stage external edit").With(Enabled(!tab->busy)).OnClick([=] { resolve(git::Resolution::Worktree); }),
      tab->can_edit_conflict ? PrimaryAction("Mark resolved", [=] { resolve(git::Resolution::Content); }, !tab->busy)
                            : View{},
    }.With(Spacing(6.0F)),
  }.With(Spacing(8.0F), Padding(12.0F), CrossAlign(CrossAxisAlignment::Stretch))).With(Grow(), ScrollBar());
}

View ConflictsPane(std::shared_ptr<Tab> tab, TaskScope tasks, Revision revision, bool compact) {
  if (!tab->operation) return InspectorPlaceholder("No conflicts", "Select History or Changes to continue.");
  const auto operation = *tab->operation;
  const auto version = operation.version;
  return Column {
    Column {
      SingleLine("Conflicts", kText, 22.0F, FontWeight::SemiBold),
      SingleLine(OperationLabel(operation.kind), kWarning, 12.0F),
    }.With(Padding(12.0F), Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    Divider(),
    LabelSection("UNRESOLVED PATHS", operation.conflicts.size()),
    operation.conflicts.empty() ? InspectorPlaceholder("All paths resolved", "Continue when you are ready.")
        : VirtualList(operation.conflicts, [=](const git::ConflictEntry& entry) {
            return SelectableRow(entry.path, tab->resolved_conflict == entry.path, [=] {
              tab->show_inspector = compact;
              tasks.Launch(LoadConflict(tab, entry, version, revision));
            }, Badge("Conflict", kWarning, kElevated)).With(Frame{.height = 34.0F}).Key(entry.path);
          }).ItemExtent(34.0F).With(Grow(), ScrollBar()).Key("conflict-files"),
    Divider(),
    tab->confirm_abort ? Column {
      Text("Abort and discard changes made during this operation?").With(Foreground(kWarning), FontSize(12.0F)),
      Flow {
        Button("Keep operation").OnClick([=] { tab->confirm_abort = false; Changed(revision); }),
        Button("Confirm abort").OnClick([=] {
          tab->confirm_abort = false;
          tasks.Launch(Change(tab, revision, tab->Repo().AbortAsync(operation.version, true), "Operation aborted", true));
        }),
      }.With(Spacing(6.0F)),
    }.With(Spacing(6.0F), Padding(10.0F), Background(kRemoved), CrossAlign(CrossAxisAlignment::Stretch)) : View{},
    Flow {
      PrimaryAction("Continue", [=] {
        tasks.Launch(Integrate(tab, revision, tab->Repo().ContinueAsync(operation.version)));
      }, operation.can_continue && !tab->busy),
      Button("Skip step").With(Enabled(operation.can_skip && !tab->busy)).OnClick([=] {
        tasks.Launch(Integrate(tab, revision, tab->Repo().SkipAsync(operation.version)));
      }),
      Button("Abort").With(Enabled(operation.can_abort && !tab->busy)).OnClick([=] {
        tab->confirm_abort = true;
        Changed(revision);
      }),
    }.With(Spacing(6.0F), Padding(10.0F), Background(kToolbar)),
  }.With(Grow(), Background(kSurface), CrossAlign(CrossAxisAlignment::Stretch));
}

View CommitComposer(std::shared_ptr<Tab> tab, TaskScope tasks, Revision revision, bool compact) {
  const bool has_staged = tab->status && std::ranges::any_of(tab->status->files, &git::FileStatus::staged);
  const bool reviewed = tab->reviewed_commit.IsValid();
  const bool can_commit = !tab->busy && !tab->operation && reviewed && !tab->message.text.empty();
  const bool can_amend = can_commit && tab->info && tab->info->head.target.has_value();
  View identity;
  if (tab->edit_identity) {
    identity = Column {
      Row {
        TextField(tab->author_name).Label("Author name").With(Grow()).OnChanged([=](const TextEditingValue& value) {
          tab->author_name = value;
          Changed(revision);
        }),
        TextField(tab->author_email).Label("Author email").With(Grow()).OnChanged([=](const TextEditingValue& value) {
          tab->author_email = value;
          Changed(revision);
        }),
      }.With(Spacing(6.0F)),
      Button("Save identity").With(Enabled(!tab->busy && !tab->author_name.text.empty() &&
                                           !tab->author_email.text.empty())).OnClick([=] {
        tasks.Launch(Change(tab, revision, tab->Repo().SetIdentityAsync({tab->author_name.text, tab->author_email.text}),
                            "Repository identity saved"));
      }),
    }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Stretch));
  }
  return Column {
    Row {
      SingleLine("Commit", kText, 15.0F, FontWeight::Bold).With(Grow()),
      reviewed ? Badge("Staged changes reviewed") : View{},
      ReviewButton(tab->edit_identity ? "Hide identity" : "Identity", [=] {
        tab->edit_identity = !tab->edit_identity;
        Changed(revision);
      }),
    }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Center)),
    identity,
    Theme(ReviewFieldStyle(kSurface, 68.0F, EdgeInsets::All(8.0F), 12.0F),
          TextField(tab->message).Label("Commit message").Placeholder("Commit message")
              .Variant(TextFieldVariant::Outlined).VerticalAlign(TextVerticalAlign::Top)
              .LineLimits(TextFieldLineLimits::MultiLine(2, 3))
              .OnChanged([=](const TextEditingValue& value) { tab->message = value; Changed(revision); })
              .Key("commit-message")),
    Row {
      ReviewButton("Review staged", [=] {
        tab->show_inspector = compact;
        tasks.Launch(ReviewIndex(tab, revision));
      }, !tab->busy && !tab->operation && (has_staged || (tab->info && tab->info->head.target))),
      Spacer(),
      ReviewButton("Amend", [=] { tasks.Launch(Commit(tab, revision, true)); }, can_amend),
      ReviewButton("Commit", [=] { tasks.Launch(Commit(tab, revision, false)); }, can_commit, true),
    }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Padding(12.0F), Spacing(8.0F), Background(kSidebar), CrossAlign(CrossAxisAlignment::Stretch));
}

View ChangesPane(std::shared_ptr<Tab> tab, TaskScope tasks, Revision revision, bool compact) {
  View file_groups = Column {
    WorkingChanges(tab, tasks, revision, false, compact),
    Divider(),
    WorkingChanges(tab, tasks, revision, true, compact),
    Divider(),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
  if (!compact) {
    file_groups = ScrollView(std::move(file_groups)).With(ScrollBar());
  }
  return Column {
    Row {
      SingleLine("Changes", kText, 25.0F, FontWeight::Bold).With(Grow()),
      Badge(std::to_string(ChangedPaths(*tab)) + " paths", kMuted, kElevated),
    }.With(Frame{.height = kPageHeadingHeight}, Padding(EdgeInsets::Symmetric(16.0F, 0.0F)),
           CrossAlign(CrossAxisAlignment::Center)),
    Divider(),
    std::move(file_groups).With(Grow()),
    Divider(),
    CommitComposer(tab, tasks, revision, compact),
  }.With(Grow(), Background(kSurface), CrossAlign(CrossAxisAlignment::Stretch));
}

View ReviewPane(std::shared_ptr<Tab> tab, TaskScope tasks, Revision revision, bool compact,
                const std::shared_ptr<huxerui::Clipboard>& clipboard) {
  if (tab->page == WorkspacePage::Conflicts) return ConflictInspector(tab, tasks, revision);
  if (tab->page == WorkspacePage::History) return CommitInspector(tab, tasks, revision, compact, clipboard);
  if (tab->selected_path.empty() && !tab->comparison_files.files.empty()) {
    return Column {
      SingleLine("Review staged changes", kText, 17.0F, FontWeight::SemiBold).With(Padding(12.0F)),
      CommitFiles(tab, tasks, revision),
    }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch));
  }
  return PatchPane(tab, tasks, revision, compact);
}

View TransferPane(std::shared_ptr<Workspace> workspace, Revision revision) {
  if (!workspace->transfer) return {};
  const auto snapshot = workspace->progress;
  View auth;
  if (snapshot.authentication) {
    const auto challenge = *snapshot.authentication;
    auth = Column {
      SingleLine(challenge.endpoint, kMuted),
      Flow {
        TextField(workspace->username).Label("Username").Placeholder(challenge.username)
            .With(Frame{.min_width = 130.0F}).OnChanged([=](const TextEditingValue& value) {
              workspace->username = value;
              Changed(revision);
            }),
        TextField(workspace->secret).Label("Token or password").Secure().With(Frame{.min_width = 130.0F})
            .OnChanged([=](const TextEditingValue& value) { workspace->secret = value; Changed(revision); }),
        PrimaryAction("Authenticate", [=] {
          if (!workspace->transfer) return;
          const auto mask = [](git::CredentialKind kind) { return static_cast<unsigned>(kind); };
          const auto kind = challenge.allowed & mask(git::CredentialKind::UserPassword)
              ? git::CredentialKind::UserPassword : challenge.allowed & mask(git::CredentialKind::SshAgent)
              ? git::CredentialKind::SshAgent : git::CredentialKind::Username;
          git::Credential credential{kind, workspace->username.text.empty() ? challenge.username : workspace->username.text,
                                     std::move(workspace->secret.text)};
          workspace->secret = {};
          const auto result = std::visit([&](const auto& job) {
            return job.AnswerAuth(challenge.id, std::move(credential));
          }, *workspace->transfer);
          if (!result.Succeeded()) workspace->notice = result.Error().message;
          Changed(revision);
        }),
      }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)),
    }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Stretch));
  }
  return Column {
    Row {
      SingleLine(workspace->transfer_label + " · " + PhaseLabel(snapshot.phase), kAccent, 12.0F,
                 FontWeight::SemiBold).With(Grow()),
      snapshot.total ? CodeLabel(std::to_string(snapshot.completed) + "/" + std::to_string(snapshot.total)) : View{},
      Button("Stop").With(Enabled(snapshot.phase != git::TransferPhase::Stopping)).OnClick([=] {
        if (workspace->transfer) std::visit([](const auto& job) { job.RequestStop(); }, *workspace->transfer);
      }),
    }.With(Spacing(7.0F), CrossAlign(CrossAxisAlignment::Center)),
    snapshot.authentication ? View{} : snapshot.total
        ? View(ProgressBar(static_cast<float>(snapshot.completed) / static_cast<float>(snapshot.total)))
        : View(ProgressBar()),
    auth,
  }.With(Padding(EdgeInsets::Symmetric(12.0F, 8.0F)), Spacing(6.0F), Background(kToolbar),
         CrossAlign(CrossAxisAlignment::Stretch));
}

void ShowRepositoryChooser(const std::shared_ptr<Workspace>& workspace, Revision revision) {
  workspace->adding_repository = true;
  workspace->repository_action = RepositoryAction::Open;
  workspace->path = {};
  workspace->remote = {};
  workspace->relocate.clear();
  workspace->notice.clear();
  workspace->show_path = false;
  workspace->focus_open = true;
  Changed(revision);
}

void ActivateTab(const std::shared_ptr<Workspace>& workspace, const std::shared_ptr<Tab>& tab,
                 const std::shared_ptr<git::GitService>& service, TaskScope tasks, Revision revision) {
  const auto it = std::ranges::find(workspace->tabs, tab);
  if (it == workspace->tabs.end()) return;
  workspace->current = static_cast<std::size_t>(it - workspace->tabs.begin());
  workspace->adding_repository = false;
  workspace->notice.clear();
  workspace->relocate.clear();
  Changed(revision);
  if (tab->repository) tasks.Launch(HydrateTab(tab, revision));
  else if (tab->open_error.empty()) tasks.Launch(OpenSavedTab(workspace, service, tab, revision));
}

void CloseTab(const std::shared_ptr<Workspace>& workspace, const std::shared_ptr<Tab>& tab,
              const std::shared_ptr<git::GitService>& service, DialogHandle dialogs, TaskScope tasks, Revision revision) {
  const auto close = [=] {
    RemoveTab(*workspace, tab);
    if (const auto active = CurrentTab(*workspace)) ActivateTab(workspace, active, service, tasks, revision);
    else ShowRepositoryChooser(workspace, revision);
  };
  if (tab->message.text.empty()) {
    close();
  } else {
    ConfirmWorkspace(workspace, dialogs, "Discard commit draft?", "Closing " + tab->name + " will discard its unsent commit message.",
                 "Discard draft", "Keep tab", close);
  }
}

void ReopenLocation(const std::shared_ptr<Workspace>& workspace, SavedLocation location,
                    const std::shared_ptr<git::GitService>& service, TaskScope tasks, Revision revision) {
  auto it = std::ranges::find_if(workspace->tabs, [&](const auto& tab) { return tab->location.id == location.id; });
  if (it == workspace->tabs.end()) {
    auto tab = std::make_shared<Tab>();
    tab->saved.location = location.id;
    tab->name = location.name;
    tab->location = std::move(location);
    workspace->tabs.push_back(std::move(tab));
    it = std::prev(workspace->tabs.end());
  }
  auto tab = *it;
  tab->open_error.clear();
  RememberLocation(*workspace, tab->location);
  ActivateTab(workspace, tab, service, tasks, revision);
}

void LocateRepository(const std::shared_ptr<Workspace>& workspace, std::string id, Revision revision) {
  ShowRepositoryChooser(workspace, revision);
  workspace->relocate = std::move(id);
  workspace->show_path = true;
  Changed(revision);
}

View RepositoryTabs(std::shared_ptr<Workspace> workspace, std::shared_ptr<git::GitService> service,
                    DialogHandle dialogs, TaskScope tasks, Revision revision, bool compact) {
  std::vector<View> tabs;
  for (std::size_t i = 0; i < workspace->tabs.size(); ++i) {
    const auto tab = workspace->tabs[i];
    const bool selected = i == workspace->current && !workspace->adding_repository;
    tabs.push_back(Row {
      SingleLine(tab->name, selected ? kText : kMuted, 12.0F,
                 selected ? FontWeight::SemiBold : FontWeight::Regular)
          .With(Frame{.max_width = compact ? 94.0F : 168.0F}, Padding(EdgeInsets::Symmetric(9.0F, 7.0F)),
                HoverFeedback(), Focusable(true), Tooltip(tab->location.path),
                Semantics{.role = SemanticRole::Tab, .label = tab->name, .selected = selected})
          .OnClick([=] { ActivateTab(workspace, tab, service, tasks, revision); }).Key("repository-tab:" + tab->location.id),
      SingleLine("×", kMuted, 15.0F).With(Padding(EdgeInsets::Symmetric(6.0F, 5.0F)), HoverFeedback(), Focusable(true),
              Semantics{.role = SemanticRole::Button, .label = "Close " + tab->name})
          .OnClick([=] { CloseTab(workspace, tab, service, dialogs, tasks, revision); }),
    }.With(Padding(EdgeInsets::Symmetric(3.0F, 0.0F)), CornerRadius(6.0F),
           Border{.color = selected ? kBorder : kToolbar},
           Background(selected ? kSurface : kToolbar), CrossAlign(CrossAxisAlignment::Center))
        .Key(tab->location.id.empty() ? tab->info->git_directory.Path() : tab->location.id));
  }
  return Row {
    RepositoryGlyph(26.0F),
    SingleLine("Lib-Git", kText, 15.0F, FontWeight::SemiBold),
    Divider(Axis::Vertical),
    ScrollView(Row(tabs).With(Spacing(5.0F), Semantics{.role = SemanticRole::TabList}))
        .ScrollAxis(Axis::Horizontal).With(Grow()),
    !workspace->tabs.empty() && !workspace->adding_repository
        ? ToolbarAction(compact ? "Add repo" : "Add repository", WorkspaceIcon::Folder,
                        [=] { ShowRepositoryChooser(workspace, revision); })
        : View{},
  }.With(Frame{.height = 49.0F}, Padding(EdgeInsets::Symmetric(11.0F, 6.0F)), Background(kToolbar),
         Spacing(9.0F), CrossAlign(CrossAxisAlignment::Center));
}

View OpenRepositoryPane(std::shared_ptr<Workspace> workspace, std::shared_ptr<git::GitService> service,
                         std::shared_ptr<FilePicker> picker, ApplicationHandle application, TaskScope tasks,
                         TaskScope app_tasks, Revision revision, ViewHandle open_focus, bool compact) {
  const auto action = workspace->repository_action;
  const bool clone = action == RepositoryAction::Clone;
  const bool init = action == RepositoryAction::Initialize;
  const bool open = !clone && !init;
  const bool can_open = service->GetCapabilities().local && !workspace->opening_repository;
  const bool ready = can_open && !workspace->path.text.empty() &&
                     (!clone || (!workspace->remote.text.empty() && !workspace->transfer));
  const bool picker_available = open && picker && picker->CanOpenDirectories(true);
  const bool show_path = !open || workspace->show_path || !picker_available;
  const auto choose = [=] { tasks.Launch(ChooseDirectory(workspace, service, picker, revision)); };
  const auto submit = [=] {
    if (!ready) return;
    std::optional<File> path;
    try {
      path.emplace(workspace->path.text);
    } catch (const std::exception& error) {
      workspace->notice = error.what();
      Changed(revision);
      return;
    }
    if (clone) {
      auto result = service->StartClone({workspace->remote.text, std::move(*path)});
      StartTransfer(workspace, {}, std::move(result), "Clone", app_tasks, revision);
    } else {
      tasks.Launch(OpenDirectory(workspace, service, std::move(*path), revision, init));
    }
  };
  const auto select_mode = [=](RepositoryAction mode) {
    workspace->repository_action = mode;
    workspace->notice.clear();
    if (mode != RepositoryAction::Open) workspace->relocate.clear();
    Changed(revision);
  };
  std::vector<View> recents;
  if (open && workspace->relocate.empty() && !workspace->recents.empty()) {
    recents.push_back(Row {SingleLine("Recent repositories", kText, 13.0F, FontWeight::SemiBold)}
        .With(Padding(10.0F), Background(kSidebar)));
    for (const auto& entry : workspace->recents) {
      const auto missing = workspace->location_errors.find(entry.id);
      const bool unavailable = missing != workspace->location_errors.end();
      recents.push_back(Divider());
      recents.push_back(Row {
        RepositoryGlyph(31.0F),
        Column {
          SingleLine(entry.name, kText, 13.0F, FontWeight::SemiBold),
          SingleLine(unavailable ? missing->second : entry.path, unavailable ? kWarning : kMuted, 11.0F),
        }.With(Grow(), Spacing(2.0F)),
        Button(unavailable ? (entry.grant ? "Grant access…" : "Locate…") : "Open")
            .With(Enabled(can_open)).OnClick([=] {
          if (unavailable) LocateRepository(workspace, entry.id, revision);
          else ReopenLocation(workspace, entry, service, tasks, revision);
        }).Key("recent:" + entry.id),
        Button("×").With(Semantics{.label = "Remove " + entry.name + " from recents"})
            .OnClick([=] {
          std::erase_if(workspace->recents, [&](const auto& item) { return item.id == entry.id; });
          if (entry.grant) workspace->forgotten_grants.push_back(entry.id);
          workspace->location_errors.erase(entry.id);
          Changed(revision);
        }),
      }.With(Padding(EdgeInsets::Symmetric(10.0F, 8.0F)), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)));
    }
  }
  View path_input;
  if (show_path) {
    auto field = TextField(workspace->path).Label(clone ? "Destination directory" : "Repository path")
        .Placeholder("Path to a local directory").Variant(TextFieldVariant::Outlined)
        .OnChanged([=](const TextEditingValue& value) { workspace->path = value; Changed(revision); })
        .OnSubmitted(submit).With(open_focus, Frame{.width = compact ? 300.0F : 580.0F}).Key("repository-path");
    path_input = std::move(field);
  }
  auto form = Column {
    Row {
      RepositoryGlyph(42.0F),
      Text(!workspace->relocate.empty() ? "Locate repository" : clone ? "Clone a repository" :
           init ? "Initialize a repository" : "Open a repository")
          .Style(TextStyle{Font::System(27.0F).WithWeight(FontWeight::SemiBold), kText}),
    }.With(Spacing(11.0F), CrossAlign(CrossAxisAlignment::Center)),
    !service->GetCapabilities().local ? Text("Repository access is unavailable in this build.").With(Foreground(kMuted))
                                      : View{},
    open ? Flow {
      picker_available ? PrimaryAction("Choose folder", choose, can_open,
                                       show_path ? std::optional<ViewHandle>{} : std::optional{open_focus}) : View{},
      workspace->relocate.empty() ? Button("Clone…").OnClick([=] { select_mode(RepositoryAction::Clone); }) : View{},
      !show_path ? Button("Enter path…").OnClick([=] {
        workspace->show_path = true;
        workspace->focus_open = true;
        Changed(revision);
      }) : View{},
    }.With(Spacing(7.0F)) : View{},
    clone ? TextField(workspace->remote).Label("Remote URL").Placeholder("https://git.example.org/team/project.git")
        .Variant(TextFieldVariant::Outlined)
        .OnChanged([=](const TextEditingValue& value) { workspace->remote = value; Changed(revision); }) : View{},
    std::move(path_input),
    show_path ? Flow {
      PrimaryAction(workspace->opening_repository ? "Opening…" : clone ? "Clone repository" :
                    init ? "Initialize repository" : "Open path", submit, ready),
      Button("App storage").With(Enabled(can_open)).OnClick([=] {
        try {
          workspace->path = TextEditingValue::FromText(application.Directories().data_directory.Child("repository").Path());
        } catch (const std::logic_error& error) {
          workspace->notice = error.what();
        }
        Changed(revision);
      }),
    }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)) : View{},
    recents.empty() ? View{} : Column(std::move(recents))
        .With(Background(kSurface), Border{.color = kBorder}, CornerRadius(8.0F), ClipChildren(),
              CrossAlign(CrossAxisAlignment::Stretch)),
    Flow {
      open && workspace->relocate.empty() ? Button("Initialize a repository…")
          .OnClick([=] { select_mode(RepositoryAction::Initialize); }) : View{},
      !open || !workspace->relocate.empty() ? Button("Open local").OnClick([=] {
        workspace->relocate.clear(); select_mode(RepositoryAction::Open);
      }) : View{},
      !workspace->tabs.empty() ? Button("Back to workspace").OnClick([=] {
        workspace->adding_repository = false;
        workspace->notice.clear();
        workspace->relocate.clear();
        Changed(revision);
      }) : View{},
    }.With(Spacing(6.0F)),
  }.With(Frame{.width = 620.0F}, Padding(compact ? 12.0F : 20.0F), Spacing(13.0F),
         CrossAlign(CrossAxisAlignment::Stretch));
  return ScrollView(Row {Spacer(), form, Spacer()}.With(CrossAlign(CrossAxisAlignment::Start)))
      .With(Padding(EdgeInsets{.top = compact ? 16.0F : 58.0F, .right = 14.0F, .bottom = 14.0F, .left = 14.0F}),
            Grow(), ScrollBar(), Background(kWindow));
}

View RepositoryToolbar(std::shared_ptr<Workspace> workspace, std::shared_ptr<Tab> tab,
                        std::shared_ptr<git::GitService> service, DialogHandle dialogs, TaskScope tasks,
                        TaskScope app_tasks, Revision revision, bool compact) {
  const auto head = tab->info->head;
  const auto upstream = tab->info->upstream;
  const auto directory = tab->info->git_directory.Path();
  const auto remote = tab->remote;
  const auto prefix = "refs/remotes/" + remote + "/";
  const auto can_transfer = [=] {
    return CurrentTab(*workspace) == tab && !workspace->adding_repository && !workspace->transfer && !tab->busy;
  };
  const bool can_fetch = can_transfer() && !tab->remotes.empty();
  const bool can_pull = can_fetch && !tab->operation && tab->info->work_directory &&
                        upstream && upstream->starts_with(prefix);
  const bool can_push = can_fetch && head.target && head.kind == git::HeadKind::Attached &&
                        !workspace->uncertain_pushes.contains(directory) && !tab->operation;
  const auto destination = upstream && upstream->starts_with(prefix)
      ? "refs/heads/" + upstream->substr(prefix.size()) : head.reference;
  auto sync = Row {
    ToolbarAction("Refresh", WorkspaceIcon::Refresh, [=] { tasks.Launch(RefreshFromDisk(tab, revision)); }, can_transfer()),
    ToolbarAction("Fetch", WorkspaceIcon::Fetch, [=] {
      if (can_transfer()) StartTransfer(workspace, tab, service->StartFetch(tab->Repo(), {.remote = remote}),
                                       "Fetch", app_tasks, revision);
    }, can_fetch),
    ToolbarAction("Pull…", WorkspaceIcon::Branch, [=] {
      if (!can_transfer() || !upstream) return;
      ConfirmWorkspace(workspace, dialogs, "Pull from " + remote + "?", "Repository  " + tab->name + "\nLocal branch  " + HeadLabel(head) +
                   "\nUpstream  " + *upstream + "\nFast-forward only", "Pull", "Cancel", [=] {
        if (can_transfer() && !tab->operation && remote == tab->remote) {
          StartTransfer(workspace, tab, service->StartPull(tab->Repo(), {{.remote = remote}, head, *upstream}),
                        "Pull", app_tasks, revision);
        }
      });
    }, can_pull).Key("sync-pull"),
    ToolbarAction("Push…", WorkspaceIcon::Push, [=] {
      if (!can_transfer() || !head.target) return;
      ConfirmWorkspace(workspace, dialogs, "Push to " + remote + "?", "Repository  " + tab->name + "\nSource  " +
                   HeadLabel(head) + " · " + head.target->hex.substr(0, 8) + "\nRemote ref  " + destination +
                   "\nNormal push", "Push", "Cancel", [=] {
        if (can_transfer() && !tab->operation && remote == tab->remote && !workspace->uncertain_pushes.contains(directory)) {
          StartTransfer(workspace, tab, service->StartPush(tab->Repo(), {remote, {{head.target, destination}}}),
                        "Push", app_tasks, revision);
        }
      });
    }, can_push).Key("sync-push"),
  }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Center));
  const auto path = tab->info->work_directory.value_or(tab->info->git_directory).Path();
  auto location = Row {
    RepositoryGlyph(35.0F),
    Column {
      SingleLine(tab->name, kText, 15.0F, FontWeight::SemiBold),
      !compact ? CodeLabel(path, kMuted, 10.0F).With(Tooltip(path)) : View{},
    }.With(Spacing(1.0F), Frame{.max_width = 270.0F}),
    Divider(Axis::Vertical).With(Frame{.height = 27.0F}),
    Glyph(WorkspaceIcon::Branch),
    SingleLine(head.kind == git::HeadKind::Detached ? "Detached HEAD · " + HeadLabel(head) : HeadLabel(head),
               kText, 12.0F, FontWeight::SemiBold),
    upstream ? SingleLine("→ " + ShortRef(*upstream), kMuted, 11.0F) :
        !tab->remotes.empty() ? Badge(remote, kMuted, kElevated) : View{},
    !compact && tab->reachability
        ? Badge("↑" + std::to_string(tab->reachability->ahead) + " ↓" + std::to_string(tab->reachability->behind) +
                (tab->reachability->shallow ? " · shallow" : "")) : View{},
  }.With(Spacing(7.0F), Grow(), CrossAlign(CrossAxisAlignment::Center));
  View reconcile;
  if (workspace->uncertain_pushes.contains(directory)) {
    reconcile = Button("Check remote refs").With(Enabled(can_transfer()))
        .OnClick([=] { tasks.Launch(CheckPush(workspace, tab, service, revision)); });
  }
  if (compact) {
    return Column {
      std::move(location).With(Grow(0.0F)), sync, reconcile,
    }.With(Spacing(7.0F), Padding(10.0F), CrossAlign(CrossAxisAlignment::Stretch), Background(kSurface));
  }
  return Row {
    location, reconcile, sync,
  }.With(Spacing(9.0F), Padding(10.0F), CrossAlign(CrossAxisAlignment::Center), Background(kSurface));
}

View WorkspaceStatus(std::shared_ptr<Workspace> workspace, const std::shared_ptr<Tab>& tab,
                     DialogHandle dialogs) {
  const auto message = !workspace->notice.empty() ? workspace->notice : tab && !tab->issue.empty()
      ? tab->issue : workspace->session_issue;
  if (!tab && message.empty() && workspace->last_transfer.empty()) return {};
  std::string state;
  if (tab) {
    const auto changes = ChangedPaths(*tab);
    state = tab->busy ? "Working" : tab->operation ? OperationLabel(tab->operation->kind) :
            !tab->info->work_directory ? "Bare repository" : !tab->status ? "Reading worktree" :
            changes == 0 ? "Clean worktree" : std::to_string(changes) + (changes == 1 ? " changed path" : " changed paths");
  }
  return Row {
    tab ? Badge(state, tab->operation ? kWarning : kMuted, kElevated) : View{},
    message.empty() ? Spacer()
        : SingleLine(message, kText, 12.0F).With(Grow(), Semantics{.live_region = SemanticLiveRegion::Polite}),
    tab && tab->info && tab->info->shallow ? Badge("Shallow", kWarning, kElevated) : View{},
    tab && !tab->last_fetch.empty() ? SingleLine(tab->last_fetch, kMuted, 11.0F) : View{},
    !workspace->last_transfer.empty() ? Button("Sync details").OnClick([=] {
      ConfirmWorkspace(workspace, dialogs, "Synchronization", workspace->last_transfer, "Done", {}, {});
    }) : View{},
  }.With(Padding(EdgeInsets::Symmetric(10.0F, 5.0F)), Spacing(8.0F), Background(kToolbar),
         CrossAlign(CrossAxisAlignment::Center));
}

[[huxerui::composable]]
View WorkspaceContent(std::optional<File> explicit_session, bool use_app_storage) {
  const auto service = UseService<git::GitService>();
  const auto picker = UseService<FilePicker>();
  const auto tasks = UseTaskScope();
  const auto app_tasks = UseApplicationTaskScope();
  const auto application = UseApplication();
  const auto window = UseWindow();
  const auto dialogs = UseDialog();
  const auto open_focus = UseViewHandle();
  const auto revision_focus = UseViewHandle();
  const auto state = UseState(std::make_shared<Workspace>());
  const std::shared_ptr<Workspace> workspace = state;
  const auto revision = UseState(std::uint64_t{0});
  static_cast<void>(static_cast<std::uint64_t>(revision));
  const auto viewport = UseViewportClass();
  const bool compact = viewport == ViewportClass::Compact;
  const bool wide = viewport == ViewportClass::Expanded;
  const auto show_sidebar = UseState(false);
  const bool sidebar_visible = show_sidebar;

  Lifecycle([=] {
    workspace->session_file = explicit_session;
    if (use_app_storage && service->GetCapabilities().local) {
      try {
        workspace->session_file = application.Directories().data_directory.Child("workspace-v1.txt");
      } catch (const std::logic_error&) {
        // The windowless fixture and unsupported hosts may have no application directories.
      }
    }
    if (workspace->session_file) tasks.Launch(LoadWorkspace(workspace, service, revision));
    else workspace->session_loaded = true;
  }, workspace);

  const auto snapshot = workspace->session_loaded && workspace->session_file
      ? git_preview::EncodeWorkspace(WorkspaceSnapshot(*workspace)) : std::string{};
  Lifecycle([=] {
    if (snapshot.empty()) return;
    workspace->pending_session = snapshot;
    if (!workspace->saving_session && workspace->saved_session != snapshot) {
      workspace->saving_session = true;
      app_tasks.Launch(SaveWorkspace(workspace, revision));
    }
  }, snapshot);

  window.OnLifecycleChanged([workspace, tasks, revision](WindowLifecycleState lifecycle) {
    if (lifecycle != WindowLifecycleState::Active || workspace->transfer) return;
    const auto tab = CurrentTab(*workspace);
    if (tab && tab->hydrated && !tab->busy) tasks.Launch(RefreshFromDisk(tab, revision, false));
  }, workspace);

  window.OnCloseRequest([=] {
    if (std::ranges::none_of(workspace->tabs, [](const auto& tab) { return !tab->message.text.empty(); })) return false;
    ConfirmWorkspace(workspace, dialogs, "Discard commit drafts?", "Closing the window will discard its unsent commit messages.",
                 "Discard drafts", "Keep window", [=] {
      for (const auto& tab : workspace->tabs) tab->message = {};
      window.Close();
    });
    return true;
  }, workspace);

  const auto active = workspace->adding_repository ? nullptr : CurrentTab(*workspace);
  const auto tab = active && active->hydrated ? active : nullptr;
  const auto close_sidebar = [show_sidebar] { show_sidebar = false; };
  const auto navigate = [=](WorkspacePage page) {
    const auto current = workspace->adding_repository ? nullptr : CurrentTab(*workspace);
    if (!current || !current->hydrated ||
        (page == WorkspacePage::Changes && !current->info->work_directory) ||
        (page == WorkspacePage::Conflicts && !current->operation)) return false;
    close_sidebar();
    SelectPage(current, page, tasks, revision);
    return true;
  };
#if defined(__APPLE__)
  constexpr KeyModifiers primary{.meta = true};
#else
  constexpr KeyModifiers primary{.control = true};
#endif
  const auto shortcut_allowed = [workspace] { return !workspace->modal_open && !workspace->menu_open; };
  const auto open = [=] {
    if (!shortcut_allowed()) return false;
    ShowRepositoryChooser(workspace, revision);
    return true;
  };
  window.OnShortcut(Key::O, primary, open, workspace);
  window.OnShortcut(Key::T, primary, open, workspace);
  window.OnShortcut(Key::W, primary, [=] {
    if (!shortcut_allowed()) return false;
    if (workspace->adding_repository && CurrentTab(*workspace)) {
      ActivateTab(workspace, CurrentTab(*workspace), service, tasks, revision);
      return true;
    }
    const auto current = CurrentTab(*workspace);
    if (!current) return false;
    CloseTab(workspace, current, service, dialogs, tasks, revision);
    return true;
  }, workspace);
  window.OnShortcut(Key::Digit1, primary, [=] { return shortcut_allowed() && navigate(WorkspacePage::History); }, workspace);
  window.OnShortcut(Key::Digit2, primary, [=] { return shortcut_allowed() && navigate(WorkspacePage::Changes); }, workspace);
  window.OnShortcut(Key::Digit3, primary, [=] { return shortcut_allowed() && navigate(WorkspacePage::Conflicts); }, workspace);
  window.OnShortcut(Key::L, primary, [=] {
    if (!shortcut_allowed() || !navigate(WorkspacePage::History)) return false;
    workspace->focus_revision = true;
    Changed(revision);
    return true;
  }, workspace);
  window.OnShortcut(Key::R, primary, [=] {
    if (!shortcut_allowed()) return false;
    const auto current = workspace->adding_repository ? nullptr : CurrentTab(*workspace);
    if (!current || !current->hydrated || current->busy || workspace->transfer) return false;
    tasks.Launch(RefreshFromDisk(current, revision));
    return true;
  }, workspace);
  const bool focus_open = workspace->focus_open && !active;
  const bool focus_revision = workspace->focus_revision && tab && tab->page == WorkspacePage::History;
  Lifecycle([=] {
    if (!focus_open && !focus_revision) return;
    tasks.Post([=] {
      if (focus_open && workspace->focus_open) {
        workspace->focus_open = false;
        open_focus.RequestFocus(true);
      }
      if (focus_revision && workspace->focus_revision) {
        workspace->focus_revision = false;
        revision_focus.RequestFocus(true);
      }
    });
  }, focus_open, focus_revision);

  std::string first_file;
  if (!compact && tab && tab->page == WorkspacePage::History && tab->commit && !tab->reading_review &&
      tab->selected_path.empty()) {
    const auto& files = tab->comparison_files.files;
    const auto first = std::ranges::find_if(files, [](const git::FileDelta& file) { return !file.binary; });
    if (first != files.end()) first_file = first->new_path.empty() ? first->old_path : first->new_path;
  }
  const auto generation = tab ? tab->review_generation : 0;
  Lifecycle([=] {
    // Keep automatic review behind a selection the user has already made.
    if (!first_file.empty() && generation == tab->review_generation && tab->selected_path.empty()) {
      tasks.Launch(ReadPatch(tab, tab->comparison, first_file, revision));
    }
  }, tab, generation, first_file);

  const bool open_first_change = !compact && tab && tab->page == WorkspacePage::Changes &&
                                 tab->select_worktree_file && tab->status;
  Lifecycle([=] {
    if (!open_first_change || tab->page != WorkspacePage::Changes || !tab->select_worktree_file ||
        !tab->selected_path.empty()) return;
    tab->select_worktree_file = false;
    const auto& files = tab->status->files;
    auto first = std::ranges::find_if(files, [](const git::FileStatus& file) {
      return file.unstaged && !file.conflicted && !file.untracked;
    });
    if (first == files.end()) {
      first = std::ranges::find_if(files, [](const git::FileStatus& file) {
        return !file.conflicted && (file.unstaged || file.staged);
      });
    }
    if (first != files.end()) {
      tasks.Launch(ReadPatch(tab, {.kind = first->unstaged ? git::DiffKind::Worktree : git::DiffKind::Staged},
                             first->path, revision));
    }
  }, tab, open_first_change);

  std::optional<git::ConflictEntry> first_conflict;
  std::optional<git::OperationVersion> conflict_version;
  if (!compact && tab && tab->page == WorkspacePage::Conflicts && tab->operation && !tab->operation->conflicts.empty() &&
      std::ranges::find(tab->operation->conflicts, tab->resolved_conflict, &git::ConflictEntry::path) ==
          tab->operation->conflicts.end()) {
    first_conflict = tab->operation->conflicts.front();
    conflict_version = tab->operation->version;
  }
  Lifecycle([=] {
    if (first_conflict && tab->operation && tab->operation->version == conflict_version &&
        std::ranges::find(tab->operation->conflicts, tab->resolved_conflict, &git::ConflictEntry::path) ==
            tab->operation->conflicts.end()) {
      tasks.Launch(LoadConflict(tab, *first_conflict, *conflict_version, revision));
    }
  }, tab, conflict_version);

  View body;
  View toolbar;
  if (!active) {
    body = OpenRepositoryPane(workspace, service, picker, application, tasks, app_tasks, revision, open_focus, compact);
  } else if (!tab) {
    body = Column {
      InspectorPlaceholder(active->open_error.empty() ? "Opening " + active->name + "…" : "Repository unavailable",
                           active->open_error.empty() ? active->location.path : active->open_error),
      !active->open_error.empty() ? Row {
        Button("Retry").OnClick([=] { tasks.Launch(OpenSavedTab(workspace, service, active, revision)); }),
        Button(active->location.grant ? "Grant access…" : "Locate…")
            .OnClick([=] { LocateRepository(workspace, active->location.id, revision); }),
      }.With(Spacing(8.0F), Padding(14.0F), MainAlign(MainAxisAlignment::Center)) : View{},
    }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch));
  } else {
    toolbar = RepositoryToolbar(workspace, tab, service, dialogs, tasks, app_tasks, revision, compact);
    auto links = Row {
      Button(sidebar_visible ? "Close sidebar" : "Browse").OnClick([=] { show_sidebar = !sidebar_visible; }),
      !sidebar_visible && compact ? Button("History").With(Semantics{.selected = tab->page == WorkspacePage::History})
          .OnClick([=] { navigate(WorkspacePage::History); }) : View{},
      !sidebar_visible && compact && tab->info->work_directory
          ? Button("Changes").With(Semantics{.selected = tab->page == WorkspacePage::Changes})
              .OnClick([=] { navigate(WorkspacePage::Changes); }) : View{},
      !sidebar_visible && compact && tab->operation
          ? Button("Conflicts").With(Semantics{.selected = tab->page == WorkspacePage::Conflicts})
              .OnClick([=] { navigate(WorkspacePage::Conflicts); }) : View{},
    }.With(Spacing(5.0F), Padding(6.0F), Background(kToolbar), CrossAlign(CrossAxisAlignment::Center));
    View page;
    switch (tab->page) {
      case WorkspacePage::History: page = HistoryPane(tab, tasks, revision, compact, wide, revision_focus); break;
      case WorkspacePage::Changes: page = ChangesPane(tab, tasks, revision, compact); break;
      case WorkspacePage::Conflicts: page = ConflictsPane(tab, tasks, revision, compact); break;
    }
    auto inspector = ReviewPane(tab, tasks, revision, compact, application.Clipboard());
    View content;
    if (compact) {
      content = tab->show_inspector ? View(Column {
        Row {
          Button(tab->page == WorkspacePage::History ? "Back to history" :
                 tab->page == WorkspacePage::Changes ? "Back to changes" : "Back to conflicts").OnClick([=] {
            tab->show_inspector = false;
            Changed(revision);
          }),
        }.With(Padding(6.0F), Background(kToolbar)),
        Divider(),
        inspector,
      }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch))) : page;
    } else {
      const bool history = tab->page == WorkspacePage::History;
      const auto left = history ? Frame{.min_height = 175.0F}
                                : Frame{.width = workspace->files_width, .min_width = 252.0F, .max_width = 475.0F};
      const auto right = history ? Frame{.min_height = 225.0F} : Frame{.min_width = 290.0F};
      const auto first = history ? Grow(workspace->history_share) : Grow(0.0F);
      const auto second = history ? Grow(1.0F - workspace->history_share) : Grow(1.0F);
      content = SplitLayout {
        std::move(page).With(left, first).Key("main"),
        std::move(inspector).With(right, second, Background(kSurface)).Key("inspector"),
      }.Orientation(history ? Axis::Vertical : Axis::Horizontal)
          .OnChanged([=](const std::vector<SplitChange>& changes) {
        if (changes.empty()) return;
        const auto first_change = std::ranges::find(changes, std::size_t{0}, &SplitChange::index);
        if (first_change == changes.end()) return;
        if (history) {
          const auto other = std::ranges::find(changes, std::size_t{1}, &SplitChange::index);
          if (other != changes.end()) {
            const auto total = first_change->grow.factor + other->grow.factor;
            if (total > 0.0F) workspace->history_share = std::clamp(first_change->grow.factor / total, 0.22F, 0.77F);
          }
        } else if (first_change->frame.width) {
          workspace->files_width = std::clamp(*first_change->frame.width, 252.0F, 475.0F);
        }
        Changed(revision);
      }).With(Grow()).Key(history ? "history-split" : "work-split");
    }
    if (wide) {
      body = Row {
        Sidebar(workspace, tab, service, tasks, app_tasks, revision, dialogs, application.Clipboard(), {})
            .With(Frame{.width = 220.0F}, Grow(0.0F)),
        Divider(Axis::Vertical),
        content,
      }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch));
    } else {
      body = Column {
        links,
        Divider(),
        sidebar_visible ? Sidebar(workspace, tab, service, tasks, app_tasks, revision, dialogs, application.Clipboard(), close_sidebar) : content,
      }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch));
    }
  }

  return Column {
    RepositoryTabs(workspace, service, dialogs, tasks, revision, compact),
    Divider(),
    toolbar,
    TransferPane(workspace, revision),
    body,
    Divider(),
    WorkspaceStatus(workspace, tab, dialogs),
  }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch), Background(kWindow));
}

} // namespace

View App() {
  return Theme(WorkspaceTheme(), WorkspaceContent({}, true));
}

// Keeps the windowless fixture independent from the desktop application's saved workspace.
View PreviewWithSession(File file) {
  return Theme(WorkspaceTheme(), WorkspaceContent(std::move(file), false));
}

const Application application{
    App,
    {
        .window = {
            .title = "Lib-Git",
            .initial_size = {1400.0F, 860.0F},
            .minimum_size = Size{780.0F, 560.0F},
        },
        .viewport_breakpoints = {.medium_width = 690.0F, .expanded_width = 1080.0F},
        .show_debug_overlay = false,
        .application_hooks = {git::Install},
    }
};
