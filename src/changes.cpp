#include "git_internal.h"

#include <algorithm>
#include <set>

namespace huxerui::git::detail {

namespace {

using Patch = Handle<git_patch, git_patch_free>;

struct VersionedBlob {
  std::optional<ObjectId> id;
  std::uint32_t mode = 0;
  bool operator==(const VersionedBlob&) const = default;
};

struct WorkingFile {
  VersionedBlob clean;
  std::string content;
  std::string raw_hash;
  bool operator==(const WorkingFile& other) const {
    return clean == other.clean && raw_hash == other.raw_hash;
  }
};

struct HunkEdits {
  std::size_t old_offset;
  DiffHunk hunk;
};

std::string BufferString(std::span<const std::byte> bytes) {
  return bytes.empty() ? std::string{} : std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

ObjectId HashBlob(std::string_view bytes) {
  git_oid id{};
  Check(git_odb_hash(&id, bytes.data(), bytes.size(), GIT_OBJECT_BLOB), "Hash file content");
  return Id(&id);
}

VersionedBlob IndexBlob(git_index* index, const std::string& path) {
  if (const auto* entry = git_index_get_bypath(index, path.c_str(), 0)) {
    return {Id(&entry->id), entry->mode};
  }
  return {};
}

VersionedBlob TreeBlob(git_tree* tree, const std::string& path) {
  if (!tree) {
    return {};
  }
  git_tree_entry* entry = nullptr;
  const int code = git_tree_entry_bypath(&entry, tree, path.c_str());
  Handle<git_tree_entry, git_tree_entry_free> captured(entry, git_tree_entry_free);
  if (code == GIT_ENOTFOUND) {
    return {};
  }
  Check(code, "Read path at HEAD");
  return {Id(git_tree_entry_id(entry)), static_cast<std::uint32_t>(git_tree_entry_filemode(entry))};
}

bool Plain(std::uint32_t mode) {
  return mode == 0 || mode == GIT_FILEMODE_BLOB || mode == GIT_FILEMODE_BLOB_EXECUTABLE;
}

std::string BlobContents(git_repository* repo, const VersionedBlob& version, std::size_t budget) {
  if (!version.id) {
    return {};
  }
  const auto oid = Parse(*version.id);
  auto odb = Make<git_odb, git_odb_free>("Open object database", git_repository_odb, repo);
  std::size_t size = 0;
  git_object_t type;
  Check(git_odb_read_header(&size, &type, odb.get(), &oid), "Inspect blob size");
  if (type != GIT_OBJECT_BLOB || size > budget) {
    Fail(ErrorCode::Unsupported, "File is not a text blob within the patch size limit");
  }
  auto blob = Make<git_blob, git_blob_free>("Read file blob", git_blob_lookup, repo, &oid);
  return size ? std::string(static_cast<const char*>(git_blob_rawcontent(blob.get())), size) : std::string{};
}

fs::path WorktreePath(git_repository* repo, const std::string& path) {
  const auto* root = git_repository_workdir(repo);
  if (!root) {
    Fail(ErrorCode::Unsupported, "This repository has no working directory");
  }
  auto directory = Utf8Path(root);
  const auto relative = Utf8Path(path);
  for (auto segment = relative.begin(); segment != relative.end(); ++segment) {
    directory /= *segment;
    if (std::next(segment) != relative.end()) {
      std::error_code error;
      const auto type = fs::symlink_status(directory, error).type();
      if (type == fs::file_type::symlink) {
        Fail(ErrorCode::PermissionDenied, "A parent of this path is a symbolic link");
      }
      if (error && error != std::errc::no_such_file_or_directory) {
        Fail(ErrorCode::Io, "Inspect working directory path: " + error.message());
      }
    }
  }
  return directory;
}

/// Reads the Git-normalized content for the patch and fingerprints the original bytes for stale review checks.
/// @param repo Repository that supplies the worktree and its attribute rules.
/// @param path Literal repository-relative path.
/// @param mode File mode from the comparison, used if the filesystem's executable bit is not authoritative.
/// @param budget Maximum number of file bytes to read.
/// @return Content and identity of the file, or an empty value for a deletion.
WorkingFile ReadWorkingFile(git_repository* repo, const std::string& path, std::uint32_t mode,
                            std::size_t budget) {
  RequireNoExternalFilters(repo, path);
  const auto full = WorktreePath(repo, path);
  std::error_code error;
  const auto info = fs::symlink_status(full, error);
  if (info.type() == fs::file_type::not_found || error == std::errc::no_such_file_or_directory) {
    return {};
  }
  if (error || !fs::is_regular_file(info)) {
    Fail(ErrorCode::Unsupported, "Partial staging requires an ordinary worktree file");
  }
#ifndef _WIN32
  auto config = Make<git_config, git_config_free>("Read file mode policy", git_repository_config_snapshot, repo);
  int use_file_mode = 1;
  const int mode_code = git_config_get_bool(&use_file_mode, config.get(), "core.filemode");
  if (mode_code != GIT_ENOTFOUND) {
    Check(mode_code, "Read file mode policy");
  }
  if (use_file_mode) {
    mode = (info.permissions() & fs::perms::owner_exec) != fs::perms::none
               ? GIT_FILEMODE_BLOB_EXECUTABLE : GIT_FILEMODE_BLOB;
  }
#endif
  const auto bytes = ReadBytes(full, budget);
  auto raw = BufferString(bytes);
  auto filters = Make<git_filter_list, git_filter_list_free>(
      "Read Git text conversion rules", git_filter_list_load, repo, nullptr, path.c_str(),
      GIT_FILTER_TO_ODB, GIT_FILTER_DEFAULT);
  std::string content = raw;
  if (filters) {
    git_buf filtered = GIT_BUF_INIT;
    const auto result = git_filter_list_apply_to_buffer(&filtered, filters.get(), raw.data(), raw.size());
    if (result == 0) {
      content.assign(filtered.ptr ? filtered.ptr : "", filtered.size);
    }
    git_buf_dispose(&filtered);
    Check(result, "Apply Git text conversion rules");
  }
  return {{HashBlob(content), mode}, std::move(content), HashBlob(raw).hex};
}

std::vector<HunkEdits> ReadHunks(git_patch* patch, std::size_t limit, bool& truncated) {
  std::vector<HunkEdits> hunks;
  std::size_t bytes = 0;
  std::uint32_t line_id = 1;
  for (std::size_t i = 0; i < git_patch_num_hunks(patch); ++i) {
    const git_diff_hunk* header = nullptr;
    std::size_t count = 0;
    Check(git_patch_get_hunk(&header, &count, patch, i), "Read patch hunk");
    const auto offset = header->old_lines ? header->old_start - 1 : header->old_start;
    HunkEdits hunk{static_cast<std::size_t>(std::max(0, offset)),
                  {static_cast<std::uint32_t>(i + 1), std::string(header->header, header->header_len), {}}};
    for (std::size_t l = 0; l < count; ++l) {
      const git_diff_line* line = nullptr;
      Check(git_patch_get_line_in_hunk(&line, patch, i, l), "Read patch line");
      if (line->content_len > limit - bytes || line_id > 100000) {
        truncated = true;
        return hunks;
      }
      bytes += line->content_len;
      auto kind = LineKind::Marker;
      if (line->origin == GIT_DIFF_LINE_CONTEXT) kind = LineKind::Context;
      if (line->origin == GIT_DIFF_LINE_ADDITION) kind = LineKind::Added;
      if (line->origin == GIT_DIFF_LINE_DELETION) kind = LineKind::Deleted;
      hunk.hunk.lines.push_back({line_id++, kind, line->old_lineno, line->new_lineno,
                                std::string(line->content, line->content_len)});
    }
    hunks.push_back(std::move(hunk));
  }
  return hunks;
}

std::vector<std::string_view> Lines(std::string_view bytes) {
  std::vector<std::string_view> lines;
  while (!bytes.empty()) {
    const auto end = bytes.find('\n');
    const auto count = end == std::string_view::npos ? bytes.size() : end + 1;
    lines.push_back(bytes.substr(0, count));
    bytes.remove_prefix(count);
  }
  return lines;
}

void RemoveIndexPath(git_index* index, const std::string& path) {
  const int code = git_index_remove_bypath(index, path.c_str());
  if (code != GIT_ENOTFOUND) {
    Check(code, "Remove path from index");
  }
}

void SetIndexBlob(git_index* index, const std::string& path, const VersionedBlob& blob) {
  if (!blob.id) {
    RemoveIndexPath(index, path);
    return;
  }
  git_index_entry entry{};
  entry.id = Parse(*blob.id);
  entry.mode = blob.mode;
  entry.path = path.c_str();
  Check(git_index_add(index, &entry), "Set index entry");
}

void ValidateIndexEntry(git_index* index, const std::string& path) {
  if (git_index_get_bypath(index, path.c_str(), 1) || git_index_get_bypath(index, path.c_str(), 2) ||
      git_index_get_bypath(index, path.c_str(), 3)) {
    Fail(ErrorCode::Conflict, "Use ResolveConflictAsync for unmerged index entries");
  }
}

void StageWhole(git_repository* repo, ChangeOutcome& outcome, std::stop_token stop,
                std::vector<std::string> paths, bool staged) {
  if (paths.empty()) {
    outcome.report.status = WriteStatus::NoChange;
    return;
  }
  if (!git_repository_workdir(repo)) {
    Fail(ErrorCode::Unsupported, "This repository has no working directory");
  }
  IndexEdit transaction(repo);
  auto* index = transaction.Edit();
  auto tree = staged ? Tree(nullptr, git_tree_free) : HeadTree(repo);
  for (auto& raw : paths) {
    if (stop.stop_requested()) {
      outcome.report = Stopped();
      return;
    }
    auto path = ValidatePath(raw);
    ValidateIndexEntry(index, path);
    if (staged) {
      auto working = WorktreePath(repo, path);
      std::error_code error;
      const auto info = fs::symlink_status(working, error);
      if (info.type() == fs::file_type::not_found || error == std::errc::no_such_file_or_directory) {
        RemoveIndexPath(index, path);
      } else {
        if (error) {
          Fail(ErrorCode::Io, "Inspect file for staging: " + error.message());
        }
        RequireNoExternalFilters(repo, path);
        Check(git_index_add_bypath(index, path.c_str()), "Stage path");
      }
    } else {
      SetIndexBlob(index, path, TreeBlob(tree.get(), path));
    }
    outcome.paths.push_back(std::move(path));
  }
  transaction.Publish();
  outcome.index = WriteEffect::Changed;
}

} // namespace

void RestoreIndexPaths(git_repository* repo, git_tree* tree, const git_strarray& paths,
                       std::stop_token stop, ChangeOutcome& outcome) {
  IndexEdit transaction(repo);
  auto* index = transaction.Edit();
  git_diff_options options = GIT_DIFF_OPTIONS_INIT;
  options.flags = GIT_DIFF_DISABLE_PATHSPEC_MATCH | GIT_DIFF_INCLUDE_TYPECHANGE;
  options.pathspec = paths;
  auto diff = Make<git_diff, git_diff_free>("Find literal index changes", git_diff_tree_to_index,
                                           repo, tree, index, &options);
  const auto count = git_diff_num_deltas(diff.get());
  if (!count) {
    outcome.report.status = WriteStatus::NoChange;
    return;
  }

  std::vector<std::pair<std::string, VersionedBlob>> entries;
  entries.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto* delta = git_diff_get_delta(diff.get(), i);
    std::string path = delta->old_file.path;
    entries.emplace_back(path, TreeBlob(tree, path));
    RemoveIndexPath(index, path);
  }
  for (const auto& [path, blob] : entries) {
    if (blob.id) {
      // libgit2 can remove a colliding parent or child when adding an index entry.
      const auto expected_count = git_index_entrycount(index) + 1;
      SetIndexBlob(index, path, blob);
      if (git_index_entrycount(index) != expected_count) {
        Fail(ErrorCode::InvalidArgument, "Select the paths that collide with the restored file too");
      }
    }
  }
  if (stop.stop_requested()) {
    outcome.report = Stopped();
    return;
  }

  transaction.Publish();
  outcome.index = WriteEffect::Changed;
}

struct StageState {
  std::string repository;
  std::string path;
  DiffKind kind;
  HeadState head;
  VersionedBlob index;
  WorkingFile working;
  VersionedBlob old_blob;
  VersionedBlob new_blob;
  std::string old_bytes;
  std::vector<HunkEdits> hunks;
  std::size_t budget = 0;
};

namespace {

/// Replays the selected edits under the Git index lock, using the current index as the write baseline.
/// @param repo Repository whose index will be edited.
/// @param outcome Index and path effects reported to the caller.
/// @param stop Stop signal checked before publishing the replacement index.
/// @param snapshot Reviewed input bytes, index entry, and patch identities.
/// @param request Action and one nonempty selection of hunk IDs or changed-line IDs.
void ApplySelection(git_repository* repo, ChangeOutcome& outcome, std::stop_token stop,
                    const StageState& snapshot, const PatchSelectionRequest& request) {
  if (snapshot.repository != git_repository_path(repo) ||
      (request.action == PatchAction::Stage ? DiffKind::Worktree : DiffKind::Staged) != snapshot.kind) {
    Fail(ErrorCode::StaleSnapshot, "This patch belongs to a different repository or comparison");
  }
  if (request.hunks.empty() == request.lines.empty()) {
    Fail(ErrorCode::InvalidArgument, "Select either hunks or changed lines");
  }

  IndexEdit transaction(repo);
  auto* index = transaction.Edit();
  ValidateIndexEntry(index, snapshot.path);
  if (IndexBlob(index, snapshot.path) != snapshot.index ||
      (snapshot.kind == DiffKind::Staged && ReadHead(repo) != snapshot.head) ||
      (snapshot.kind == DiffKind::Worktree &&
       ReadWorkingFile(repo, snapshot.path, snapshot.new_blob.mode, snapshot.budget) != snapshot.working)) {
    Fail(ErrorCode::StaleSnapshot, "The index or source content changed. Review a new diff before applying edits");
  }

  // A staged patch is undone by preserving the complement of the selection against HEAD.
  std::set<std::uint32_t> requested(request.hunks.begin(), request.hunks.end());
  if (request.hunks.empty()) {
    requested.insert(request.lines.begin(), request.lines.end());
  }
  const auto original = Lines(snapshot.old_bytes);
  std::string result;
  result.reserve(snapshot.old_bytes.size());
  std::size_t cursor = 0;
  bool applied_all = true;
  bool applied_any = false;
  for (const auto& entry : snapshot.hunks) {
    const bool selected_hunk = !request.hunks.empty() && requested.erase(entry.hunk.id) != 0;
    if (entry.old_offset > original.size() || entry.old_offset < cursor) {
      Fail(ErrorCode::Git, "Invalid hunk range in the reviewed patch");
    }
    while (cursor < entry.old_offset) {
      result.append(original[cursor++]);
    }
    for (const auto& line : entry.hunk.lines) {
      if (line.kind == LineKind::Marker) {
        continue;
      }
      if (line.kind == LineKind::Context) {
        result.append(original.at(cursor++));
        continue;
      }
      const bool selected = selected_hunk || (request.hunks.empty() && requested.erase(line.id) != 0);
      const bool apply = request.action == PatchAction::Stage ? selected : !selected;
      applied_any |= apply;
      applied_all &= apply;
      if (line.kind == LineKind::Deleted) {
        if (!apply) {
          result.append(original.at(cursor));
        }
        ++cursor;
      } else if (apply) {
        result.append(line.text);
      }
    }
  }
  if (!requested.empty()) {
    Fail(ErrorCode::InvalidArgument, "A selected hunk or line is not part of this patch");
  }
  while (cursor < original.size()) {
    result.append(original[cursor++]);
  }
  if (stop.stop_requested()) {
    outcome.report = Stopped();
    return;
  }

  if ((!applied_any && !snapshot.old_blob.id) || (applied_all && !snapshot.new_blob.id)) {
    RemoveIndexPath(index, snapshot.path);
  } else {
    git_index_entry entry{};
    entry.mode = snapshot.index.mode ? snapshot.index.mode : snapshot.new_blob.mode ? snapshot.new_blob.mode
                                                                                   : snapshot.old_blob.mode;
    entry.path = snapshot.path.c_str();
    Check(git_index_add_from_buffer(index, &entry, result.data(), result.size()), "Stage selected edits");
  }
  transaction.Publish();
  outcome.paths.push_back(snapshot.path);
  outcome.index = WriteEffect::Changed;
}

void WriteCommit(git_repository* repo, CommitOutcome& outcome, std::stop_token stop,
                 const std::shared_ptr<const VersionState>& expected, const CommitRequest& request, bool amend) {
  RequireString(request.message, "Commit message");
  if (Operation(repo)) {
    Fail(ErrorCode::Conflict, "Use ContinueAsync to finish the repository operation");
  }
  if (!request.signer) {
    RequireUnsignedCommitPolicy(repo);
  }
  RequireNoClientHooks(repo, {"pre-commit", "prepare-commit-msg", "commit-msg", "post-commit"});
  git_oid tree_oid{};
  {
    IndexEdit lock(repo);
    if (!expected || !MatchesVersion(repo, *expected)) {
      Fail(ErrorCode::StaleSnapshot, "The commit review no longer matches HEAD and the index");
    }
    auto index = ReadIndex(repo);
    if (git_index_has_conflicts(index.get())) {
      Fail(ErrorCode::Conflict, "Resolve the index conflicts before committing");
    }
    Check(git_index_write_tree(&tree_oid, index.get()), "Write commit tree");
  }
  const auto& head = expected->head;
  auto tree = Make<git_tree, git_tree_free>("Read commit tree", git_tree_lookup, repo, &tree_oid);
  auto committer = MakeSignature(repo, request.committer);
  Commit original(nullptr, git_commit_free);
  if (head.target) {
    original = FindCommit(repo, *head.target);
  }
  if (amend && !original) {
    Fail(ErrorCode::NotFound, "There is no commit to amend");
  }
  if (!amend && !request.allow_empty &&
      ((original && git_oid_equal(git_commit_tree_id(original.get()), &tree_oid)) ||
       (!original && git_tree_entrycount(tree.get()) == 0))) {
    outcome.report.status = WriteStatus::NoChange;
    return;
  }
  auto author = amend && !request.author ? decltype(committer)(nullptr, git_signature_free)
                                        : MakeSignature(repo, request.author);
  std::vector<Commit> parents;
  if (amend) {
    for (unsigned i = 0; i < git_commit_parentcount(original.get()); ++i) {
      parents.push_back(Make<git_commit, git_commit_free>("Read amend parent", git_commit_parent, original.get(), i));
    }
  } else if (original) {
    parents.push_back(FindCommit(repo, *head.target));
  }
  std::vector<const git_commit*> parent_handles;
  for (const auto& parent : parents) {
    parent_handles.push_back(parent.get());
  }
  if (stop.stop_requested()) {
    outcome.report = Stopped();
    return;
  }
  std::string unsigned_content;
  std::optional<std::string> signature;
  const auto* commit_author = author ? author.get() : git_commit_author(original.get());
  if (request.signer) {
    struct CommitBuffer {
      git_buf value = GIT_BUF_INIT;
      ~CommitBuffer() { git_buf_dispose(&value); }
    } buffer;
    Check(git_commit_create_buffer(&buffer.value, repo, commit_author, committer.get(), nullptr,
        request.message.c_str(), tree.get(), parent_handles.size(), parent_handles.data()),
        "Prepare the unsigned commit");
    unsigned_content.assign(buffer.value.ptr, buffer.value.size);
    auto signed_commit = request.signer(unsigned_content);
    if (!signed_commit.Succeeded()) {
      throw Failure{signed_commit.Error()};
    }
    signature = std::move(signed_commit).Value();
    RequireString(*signature, "Commit signature");
  }
  if (stop.stop_requested()) {
    outcome.report = Stopped();
    return;
  }

  // A signing provider can take time. Reacquire Git's locks and validate again before publication.
  IndexEdit lock(repo);
  auto transaction = Make<git_transaction, git_transaction_free>("Create reference transaction", git_transaction_new, repo);
  Check(git_transaction_lock_ref(transaction.get(), "HEAD"), "Lock HEAD");
  if (head.kind != HeadKind::Detached) {
    Check(git_transaction_lock_ref(transaction.get(), head.reference.c_str()), "Lock the current branch");
  }
  if (Operation(repo) || !MatchesVersion(repo, *expected)) {
    Fail(ErrorCode::StaleSnapshot, "HEAD, index, or operation state changed before commit publication");
  }
  git_oid oid{};
  if (signature) {
    Check(git_commit_create_with_signature(&oid, repo, unsigned_content.c_str(), signature->c_str(), "gpgsig"),
          "Create signed commit");
  } else {
    Check(git_commit_create(&oid, repo, nullptr, commit_author,
        committer.get(), nullptr, request.message.c_str(), tree.get(), parent_handles.size(), parent_handles.data()),
        "Create commit");
  }
  outcome.created_commit = Id(&oid);
  Check(git_transaction_set_target(transaction.get(), head.reference.c_str(), &oid, committer.get(),
                                  amend ? "commit (amend)" : "commit"),
        "Prepare the HEAD update");
  outcome.head = WriteEffect::Unknown;
  Check(git_transaction_commit(transaction.get()), "Publish commit reference");
  outcome.head = WriteEffect::Changed;
}

} // namespace

} // namespace huxerui::git::detail

namespace huxerui::git {
using namespace detail;

Task<GitResult<FileDiff>> Repository::ReadPatchAsync(PatchQuery query) const {
  const bool immutable = query.comparison.kind == DiffKind::Commit;
  return Query(state_, [query = std::move(query)](git_repository* repo, std::stop_token) {
    const auto path = ValidatePath(query.path);
    if (!query.max_bytes || query.max_bytes > 32 * 1024 * 1024) {
      Fail(ErrorCode::InvalidArgument, "A patch byte limit must be within 1–32 MiB");
    }
    auto comparison = query.comparison;
    if (!comparison.detect_renames) {
      comparison.paths = {path};
    }
    auto comparison_diff = MakeDiff(repo, comparison);
    std::size_t index = 0;
    const git_diff_delta* match = nullptr;
    for (; index < git_diff_num_deltas(comparison_diff.get()); ++index) {
      const auto* delta = git_diff_get_delta(comparison_diff.get(), index);
      if ((delta->new_file.path && delta->new_file.path == path) ||
          (delta->old_file.path && delta->old_file.path == path)) {
        match = delta;
        break;
      }
    }
    if (!match) {
      Fail(ErrorCode::NotFound, "The selected file is no longer part of the comparison");
    }

    FileDiff result;
    result.file = ReadDelta(*match);
    if (match->old_file.size > query.max_bytes || match->new_file.size > query.max_bytes) {
      result.truncated = true;
      return result;
    }
    Patch patch(nullptr, git_patch_free);
    std::shared_ptr<StageState> snapshot;
    std::string next;
    if (comparison.kind != DiffKind::Commit && Plain(result.file.old_mode) && Plain(result.file.new_mode) &&
        result.file.kind != DeltaKind::Renamed && result.file.kind != DeltaKind::Copied &&
        result.file.kind != DeltaKind::Conflicted &&
        (!result.file.old_mode || !result.file.new_mode || result.file.old_mode == result.file.new_mode)) {
      try {
        snapshot = std::make_shared<StageState>();
        snapshot->repository = git_repository_path(repo);
        snapshot->path = path;
        snapshot->kind = comparison.kind;
        snapshot->head = ReadHead(repo);
        snapshot->budget = query.max_bytes;
        snapshot->index = IndexBlob(ReadIndex(repo).get(), path);
        if (comparison.kind == DiffKind::Worktree) {
          snapshot->working = ReadWorkingFile(repo, path, result.file.new_mode, query.max_bytes);
          snapshot->old_blob = snapshot->index;
          snapshot->new_blob = snapshot->working.clean;
          next = snapshot->working.content;
          snapshot->working.content.clear();
        } else {
          snapshot->old_blob = TreeBlob(HeadTree(repo).get(), path);
          snapshot->new_blob = snapshot->index;
          next = BlobContents(repo, snapshot->new_blob, query.max_bytes);
        }
        snapshot->old_bytes = BlobContents(repo, snapshot->old_blob, query.max_bytes);
        patch = Make<git_patch, git_patch_free>("Build reviewed file patch", git_patch_from_buffers,
            snapshot->old_bytes.data(), snapshot->old_bytes.size(), path.c_str(), next.data(), next.size(),
            path.c_str(), nullptr);
      } catch (const Failure& error) {
        if (error.error.code != ErrorCode::Unsupported) {
          throw;
        }
        snapshot.reset();
      }
    }
    if (!patch) {
      patch = Make<git_patch, git_patch_free>("Build file patch", git_patch_from_diff, comparison_diff.get(), index);
    }
    if (patch) {
      result.file.binary = (git_patch_get_delta(patch.get())->flags & GIT_DIFF_FLAG_BINARY) != 0;
      auto hunks = ReadHunks(patch.get(), query.max_bytes, result.truncated);
      for (const auto& hunk : hunks) {
        result.hunks.push_back(hunk.hunk);
      }
      if (snapshot && !result.file.binary && !result.truncated && !hunks.empty()) {
        snapshot->hunks = std::move(hunks);
        result.staging = StagingSnapshot(std::move(snapshot));
      }
    }
    return result;
  }, immutable);
}

Task<ChangeOutcome> Repository::StageFilesAsync(std::vector<std::string> paths) const {
  return Write<ChangeOutcome>(state_, [paths = std::move(paths)](auto* repo, auto& outcome, auto stop) mutable {
    StageWhole(repo, outcome, stop, std::move(paths), true);
  });
}

Task<ChangeOutcome> Repository::UnstageFilesAsync(std::vector<std::string> paths) const {
  return Write<ChangeOutcome>(state_, [paths = std::move(paths)](auto* repo, auto& outcome, auto stop) mutable {
    StageWhole(repo, outcome, stop, std::move(paths), false);
  });
}

Task<ChangeOutcome> Repository::ApplySelectionAsync(PatchSelectionRequest request) const {
  return Write<ChangeOutcome>(state_, [request = std::move(request)](auto* repo, auto& outcome, auto stop) {
    if (!request.snapshot.state_) {
      Fail(ErrorCode::InvalidArgument, "Select edits from a stageable patch");
    }
    ApplySelection(repo, outcome, stop, *request.snapshot.state_, request);
  });
}

Task<CommitOutcome> Repository::CommitAsync(CommitRequest request) const {
  return Write<CommitOutcome>(state_, [request = std::move(request)](auto* repo, auto& outcome, auto stop) {
    WriteCommit(repo, outcome, stop, request.expected.state_, request, false);
  });
}

Task<CommitOutcome> Repository::AmendAsync(CommitRequest request) const {
  return Write<CommitOutcome>(state_, [request = std::move(request)](auto* repo, auto& outcome, auto stop) {
    WriteCommit(repo, outcome, stop, request.expected.state_, request, true);
  });
}

} // namespace huxerui::git
