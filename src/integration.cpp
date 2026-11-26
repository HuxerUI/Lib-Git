#include "git_internal.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace huxerui::git::detail {
namespace {

using Rebase = Handle<git_rebase, git_rebase_free>;
using Annotated = Handle<git_annotated_commit, git_annotated_commit_free>;

git_checkout_options CheckoutOptions(bool force = false) {
  git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
  options.checkout_strategy = force ? GIT_CHECKOUT_FORCE : GIT_CHECKOUT_SAFE;
  return options;
}

void CheckCheckout(int code, std::string_view action) {
  if (code == GIT_ECONFLICT) {
    Fail(ErrorCode::LocalChangesWouldBeOverwritten, std::string(action) + " would overwrite local changes");
  }
  Check(code, action);
}

void RequireIdle(git_repository* repo) {
  if (Operation(repo)) {
    Fail(ErrorCode::Busy, "Finish the current Git operation first");
  }
}

void RequireClean(git_repository* repo) {
  git_status_options options = GIT_STATUS_OPTIONS_INIT;
  options.flags = GIT_STATUS_OPT_NO_REFRESH | GIT_STATUS_OPT_INCLUDE_UNTRACKED;
  auto status = Make<git_status_list, git_status_list_free>("Check worktree before integration",
                                                           git_status_list_new, repo, &options);
  if (git_status_list_entrycount(status.get())) {
    Fail(ErrorCode::LocalChangesWouldBeOverwritten, "Commit or stash local changes before starting this operation");
  }
}

std::optional<ObjectId> ReadOptionalRef(git_repository* repo, const char* name) {
  git_oid oid{};
  const int code = git_reference_name_to_id(&oid, repo, name);
  if (code == GIT_ENOTFOUND) {
    return {};
  }
  Check(code, "Read operation reference");
  return Id(&oid);
}

std::optional<ConflictSide> Side(const git_index_entry* entry) {
  return entry ? std::optional(ConflictSide{entry->path, Id(&entry->id), entry->mode}) : std::nullopt;
}

std::vector<ConflictEntry> ReadConflicts(git_repository* repo) {
  auto index = ReadIndex(repo);
  auto cursor = Make<git_index_conflict_iterator, git_index_conflict_iterator_free>(
      "Read index conflicts", git_index_conflict_iterator_new, index.get());
  std::vector<ConflictEntry> entries;
  for (;;) {
    const git_index_entry* base = nullptr;
    const git_index_entry* ours = nullptr;
    const git_index_entry* theirs = nullptr;
    const int code = git_index_conflict_next(&base, &ours, &theirs, cursor.get());
    if (code == GIT_ITEROVER) {
      break;
    }
    Check(code, "Read conflict stages");
    entries.push_back({ours ? ours->path : theirs ? theirs->path : base->path, Side(base), Side(ours), Side(theirs)});
  }
  return entries;
}

std::string ReadMessage(git_repository* repo) {
  git_buf buffer = GIT_BUF_INIT;
  const int code = git_repository_message(&buffer, repo);
  std::string message;
  if (code == 0 && buffer.ptr) {
    message.assign(buffer.ptr, buffer.size);
  }
  git_buf_dispose(&buffer);
  if (code != GIT_ENOTFOUND) {
    Check(code, "Read integration message");
  }
  return message;
}

void PopulateOperation(git_repository* repo, IntegrationOutcome& outcome) {
  outcome.operation = ReadOperation(repo);
  if (outcome.operation && !outcome.operation->conflicts.empty()) {
    outcome.report.status = WriteStatus::NeedsResolution;
  }
}

Rebase OpenRebase(git_repository* repo) {
  return Make<git_rebase, git_rebase_free>("Reopen linear rebase", git_rebase_open, repo, nullptr);
}

bool CanOpenRebase(git_repository* repo) {
  git_rebase* value = nullptr;
  const int code = git_rebase_open(&value, repo, nullptr);
  Rebase rebase(value, git_rebase_free);
  return code == 0;
}

void Replay(git_repository* repo, git_rebase* rebase, IntegrationOutcome& outcome,
             std::stop_token stop, bool resume, const std::optional<std::string>& message = {}) {
  auto committer = MakeSignature(repo, {});
  while (true) {
    if (stop.stop_requested()) {
      outcome.report = Stopped();
      outcome.operation = ReadOperation(repo);
      return;
    }
    if (!resume) {
      git_rebase_operation* operation = nullptr;
      const int code = git_rebase_next(&operation, rebase);
      if (code == GIT_ITEROVER) {
        Check(git_rebase_finish(rebase, committer.get()), "Finish rebase");
        outcome.commit = ReadHead(repo).target;
        outcome.operation.reset();
        return;
      }
      Check(code, "Apply rebase step");
    }
    if (git_index_has_conflicts(ReadIndex(repo).get())) {
      PopulateOperation(repo, outcome);
      return;
    }
    git_oid oid{};
    const int code = git_rebase_commit(&oid, rebase, nullptr, committer.get(), nullptr,
                                       resume && message ? message->c_str() : nullptr);
    if (code != GIT_EAPPLIED) {
      Check(code, "Commit rebase step");
    }
    resume = false;
  }
}

void BeginRebase(git_repository* repo, const ObjectId& upstream, IntegrationOutcome& outcome,
                 std::stop_token stop) {
  const auto oid = Parse(upstream);
  auto tip = Make<git_annotated_commit, git_annotated_commit_free>("Read rebase target",
                                                                git_annotated_commit_lookup, repo, &oid);
  git_rebase_options options = GIT_REBASE_OPTIONS_INIT;
  options.checkout_options = CheckoutOptions();
  auto rebase = Make<git_rebase, git_rebase_free>("Start linear rebase", git_rebase_init,
                                                 repo, nullptr, tip.get(), nullptr, &options);
  Replay(repo, rebase.get(), outcome, stop, false);
}

OperationSnapshot RequireOperation(git_repository* repo, const OperationVersion& expected) {
  auto state = ReadOperation(repo);
  if (!state || expected.value.empty() || state->version != expected) {
    Fail(ErrorCode::StaleSnapshot, "The Git operation changed. Reload it before continuing");
  }
  return *state;
}

void CheckHead(git_repository* repo, const HeadState& expected) {
  if (expected != ReadHead(repo)) {
    Fail(ErrorCode::StaleSnapshot, "HEAD changed before the command started");
  }
}

void CheckWorktreeVersion(git_repository* repo, const std::optional<WorktreeVersion>& expected) {
  if (!expected || expected->value.empty() || ReadWorktreeVersion(repo) != *expected) {
    Fail(ErrorCode::StaleSnapshot, "Review the current working directory before discarding changes");
  }
}

std::string LocalRelativePath(git_repository* repo, std::string_view input) {
  auto path = ValidatePath(input);
  const auto* root = git_repository_workdir(repo);
  if (!root) {
    Fail(ErrorCode::Unsupported, "This repository has no working directory");
  }
  std::error_code error;
  const auto base = fs::weakly_canonical(Utf8Path(root), error);
  const auto parent = fs::weakly_canonical((base / Utf8Path(path)).parent_path(), error);
  if (error || !IsWithin(base, parent)) {
    Fail(ErrorCode::PermissionDenied, "The selected path is outside the worktree");
  }
  return path;
}

void CheckCleanAttributeFile(git_repository* repo, const git_tree_entry* entry) {
  const auto oid = *git_tree_entry_id(entry);
  auto blob = Make<git_blob, git_blob_free>("Read checkout attributes", git_blob_lookup, repo, &oid);
  if (git_blob_rawsize(blob.get()) == 0) {
    return;
  }
  const auto text = std::string_view(static_cast<const char*>(git_blob_rawcontent(blob.get())),
                                    git_blob_rawsize(blob.get()));
  if (text.find("filter=") != std::string_view::npos || text.find("working-tree-encoding=") != std::string_view::npos) {
    Fail(ErrorCode::Unsupported, "The target tree requires an external Git filter or encoding backend");
  }
}

} // namespace

WorktreeVersion ReadWorktreeVersion(git_repository* repo) {
  const auto* root = git_repository_workdir(repo);
  if (!root) {
    Fail(ErrorCode::Unsupported, "This repository has no working directory");
  }
  const auto precondition = ReadVersion(repo);
  git_status_options options = GIT_STATUS_OPTIONS_INIT;
  options.flags = GIT_STATUS_OPT_NO_REFRESH | GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                  GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS | GIT_STATUS_OPT_INCLUDE_IGNORED |
                  GIT_STATUS_OPT_RECURSE_IGNORED_DIRS;
  auto status = Make<git_status_list, git_status_list_free>("Inspect worktree changes", git_status_list_new, repo,
                                                           &options);
  std::string version = precondition->git_directory + '\n' + precondition->head.reference + '\n' +
                        (precondition->head.target ? precondition->head.target->hex : "") + '\n' + precondition->index;
  for (std::size_t i = 0; i < git_status_list_entrycount(status.get()); ++i) {
    const auto* entry = git_status_byindex(status.get(), i);
    const auto* delta = entry->index_to_workdir;
    if (!delta) {
      continue;
    }
    const auto path = LocalRelativePath(repo, delta->new_file.path ? delta->new_file.path : delta->old_file.path);
    const auto file = Utf8Path(root) / Utf8Path(path);
    std::error_code error;
    const auto info = fs::symlink_status(file, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      Fail(ErrorCode::Io, "Could not inspect worktree content: " + error.message());
    }
    version += '\n' + std::to_string(path.size()) + ":" + path + ':' + std::to_string(entry->status) +
               ':' + std::to_string(static_cast<unsigned>(info.permissions()));
    if (fs::is_regular_file(info)) {
      version += ':' + Fingerprint(file);
    } else if (fs::is_symlink(info)) {
      version += ':' + PathString(fs::read_symlink(file, error));
      if (error) {
        Fail(ErrorCode::Io, "Could not read worktree symbolic link");
      }
    }
  }
  if (!MatchesVersion(repo, *precondition)) {
    Fail(ErrorCode::StaleSnapshot, "HEAD or index changed while inspecting the working directory");
  }
  git_oid digest{};
  Check(git_odb_hash(&digest, version.data(), version.size(), GIT_OBJECT_BLOB), "Hash working directory review");
  return {Id(&digest).hex};
}

void RequireCheckoutFilters(git_repository* repo, git_tree* tree, const std::vector<std::string>& paths) {
  for (const auto& path : paths) {
    RequireNoExternalFilters(repo, path);
  }
  if (!tree) {
    return;
  }
  struct Walk {
    git_repository* repo;
    std::exception_ptr failure;
  } walk{repo, {}};
  const int code = git_tree_walk(tree, GIT_TREEWALK_PRE,
      [](const char*, const git_tree_entry* entry, void* payload) -> int {
        auto& state = *static_cast<Walk*>(payload);
        if (std::string_view(git_tree_entry_name(entry)) != ".gitattributes" ||
            git_tree_entry_type(entry) != GIT_OBJECT_BLOB) {
          return 0;
        }
        try {
          CheckCleanAttributeFile(state.repo, entry);
          return 0;
        } catch (...) {
          state.failure = std::current_exception();
          return GIT_EUSER;
        }
      }, &walk);
  if (walk.failure) {
    std::rethrow_exception(walk.failure);
  }
  Check(code, "Inspect target tree attributes");
}

std::optional<OperationKind> Operation(git_repository* repo) {
  switch (git_repository_state(repo)) {
    case GIT_REPOSITORY_STATE_NONE: return {};
    case GIT_REPOSITORY_STATE_MERGE: return OperationKind::Merge;
    case GIT_REPOSITORY_STATE_REBASE_MERGE:
    case GIT_REPOSITORY_STATE_REBASE_INTERACTIVE:
    case GIT_REPOSITORY_STATE_REBASE: return OperationKind::Rebase;
    case GIT_REPOSITORY_STATE_CHERRYPICK:
    case GIT_REPOSITORY_STATE_CHERRYPICK_SEQUENCE: return OperationKind::CherryPick;
    case GIT_REPOSITORY_STATE_REVERT:
    case GIT_REPOSITORY_STATE_REVERT_SEQUENCE: return OperationKind::Revert;
    default: return OperationKind::Other;
  }
}

std::optional<OperationSnapshot> ReadOperation(git_repository* repo) {
  const auto kind = Operation(repo);
  if (!kind) {
    return {};
  }
  OperationSnapshot snapshot;
  snapshot.kind = *kind;
  snapshot.conflicts = ReadConflicts(repo);
  const auto head = ReadHead(repo);
  const auto path = Utf8Path(git_repository_path(repo));

  // Continuation depends on the sequencer files. Destructive actions must also detect local edits after review.
  std::string version = PathString(path) + '\n' + std::to_string(git_repository_state(repo)) + '\n' +
                        head.reference + '\n' + (head.target ? head.target->hex : "") + '\n' +
                        Fingerprint(IndexPath(repo));
  for (const auto* file : {"MERGE_HEAD", "ORIG_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "MERGE_MSG",
                           "rebase-merge/msgnum", "rebase-merge/end", "rebase-merge/orig-head",
                           "rebase-merge/onto", "sequencer/todo"}) {
    version += '\n' + Fingerprint(path / file);
  }
  version += '\n' + ReadWorktreeVersion(repo).value;
  snapshot.version.value = std::move(version);
  const auto state = git_repository_state(repo);
  if (snapshot.kind == OperationKind::Rebase) {
    if (CanOpenRebase(repo)) {
      auto rebase = OpenRebase(repo);
      snapshot.step_count = git_rebase_operation_entrycount(rebase.get());
      const auto step = git_rebase_operation_current(rebase.get());
      if (step != GIT_REBASE_NO_OPERATION) {
        snapshot.step = step + 1;
        snapshot.current_commit = Id(&git_rebase_operation_byindex(rebase.get(), step)->id);
      }
      snapshot.original_head = OptionalId(git_rebase_orig_head_id(rebase.get()));
      snapshot.can_abort = true;
      snapshot.can_continue = snapshot.conflicts.empty();
      snapshot.can_skip = snapshot.step != 0;
    } else {
      snapshot.backend = Backend::GitCli;
    }
  } else {
    if (state == GIT_REPOSITORY_STATE_MERGE) {
      snapshot.original_head = ReadOptionalRef(repo, "ORIG_HEAD");
    } else if (state == GIT_REPOSITORY_STATE_CHERRYPICK || state == GIT_REPOSITORY_STATE_REVERT) {
      snapshot.current_commit = ReadOptionalRef(repo, state == GIT_REPOSITORY_STATE_CHERRYPICK
                                                        ? "CHERRY_PICK_HEAD" : "REVERT_HEAD");
      snapshot.original_head = head.target;
    }
    snapshot.can_abort = snapshot.original_head.has_value();
    snapshot.can_continue = snapshot.conflicts.empty() && snapshot.can_abort;
  }
  return snapshot;
}

void SetHeadTarget(git_repository* repo, const HeadState& expected, const git_oid* target,
                   const char* log, const git_signature* signature) {
  auto transaction = Make<git_transaction, git_transaction_free>("Create reference transaction", git_transaction_new, repo);
  Check(git_transaction_lock_ref(transaction.get(), "HEAD"), "Lock HEAD");
  if (expected.kind != HeadKind::Detached) {
    Check(git_transaction_lock_ref(transaction.get(), expected.reference.c_str()), "Lock the current branch");
  }
  CheckHead(repo, expected);
  Check(git_transaction_set_target(transaction.get(), expected.reference.c_str(), target, signature, log),
        "Prepare reference update");
  Check(git_transaction_commit(transaction.get()), "Update repository reference");
}

void SafeFastForward(git_repository* repo, const HeadState& head, git_commit* target) {
  auto tree = CommitTree(target);
  RequireCheckoutFilters(repo, tree.get());
  auto options = CheckoutOptions();
  CheckHead(repo, head);
  CheckCheckout(git_checkout_tree(repo, reinterpret_cast<git_object*>(tree.get()), &options), "Fast-forward checkout");
  SetHeadTarget(repo, head, git_commit_id(target), "merge: fast-forward");
}

void CommitIntegration(git_repository* repo, std::optional<std::string> message, IntegrationOutcome& outcome,
                       const std::optional<HeadState>& expected) {
  RequireUnsignedCommitPolicy(repo);
  RequireNoClientHooks(repo, {"pre-commit", "prepare-commit-msg", "commit-msg", "post-commit"});
  const auto head = ReadHead(repo);
  if (expected && *expected != head) Fail(ErrorCode::StaleSnapshot, "HEAD changed before publishing the integration");
  auto index = ReadIndex(repo);
  if (git_index_has_conflicts(index.get())) {
    PopulateOperation(repo, outcome);
    return;
  }
  if (!head.target) {
    Fail(ErrorCode::NotFound, "Integration commit requires a current commit");
  }
  auto current = FindCommit(repo, *head.target);
  std::vector<Commit> parents;
  parents.push_back(std::move(current));
  if (Operation(repo) == OperationKind::Merge) {
    std::vector<git_oid> additional;
    std::exception_ptr callback_error;
    struct Payload { std::vector<git_oid>& ids; std::exception_ptr& error; } payload{additional, callback_error};
    const int result = git_repository_mergehead_foreach(repo, [](const git_oid* oid, void* data) -> int {
      auto& state = *static_cast<Payload*>(data);
      try {
        state.ids.push_back(*oid);
        return 0;
      } catch (...) {
        state.error = std::current_exception();
        return GIT_EUSER;
      }
    }, &payload);
    if (callback_error) {
      std::rethrow_exception(callback_error);
    }
    Check(result, "Read merge parents");
    for (const auto& oid : additional) {
      parents.push_back(FindCommit(repo, Id(&oid)));
    }
  }
  git_oid tree_id{};
  Check(git_index_write_tree(&tree_id, index.get()), "Write integration tree");
  auto tree = Make<git_tree, git_tree_free>("Read integration tree", git_tree_lookup, repo, &tree_id);
  auto committer = MakeSignature(repo, {});
  auto picked = ReadOptionalRef(repo, "CHERRY_PICK_HEAD");
  auto original = picked ? FindCommit(repo, *picked) : Commit(nullptr, git_commit_free);
  auto text = message.value_or(ReadMessage(repo));
  if (text.empty()) {
    text = "Complete Git integration";
  }
  RequireString(text, "Commit message");
  std::vector<const git_commit*> parent_handles;
  for (const auto& parent : parents) {
    parent_handles.push_back(parent.get());
  }
  git_oid oid{};
  Check(git_commit_create(&oid, repo, nullptr, original ? git_commit_author(original.get()) : committer.get(),
      committer.get(), nullptr, text.c_str(), tree.get(), parent_handles.size(), parent_handles.data()),
      "Create integration commit");
  outcome.commit = Id(&oid);
  SetHeadTarget(repo, head, &oid, "complete integration", committer.get());
  Check(git_repository_state_cleanup(repo), "Clean completed integration state");
}

IntegrationOutcome Integrate(git_repository* repo, const ObjectId& target, PullMode mode, std::stop_token stop,
                             const std::optional<HeadState>& expected) {
  IntegrationOutcome outcome;
  RequireIdle(repo);
  if (expected) CheckHead(repo, *expected);
  auto commit = FindCommit(repo, target);
  auto annotated = Make<git_annotated_commit, git_annotated_commit_free>("Read integration target",
      git_annotated_commit_lookup, repo, git_commit_id(commit.get()));
  const git_annotated_commit* heads[]{annotated.get()};
  git_merge_analysis_t analysis;
  git_merge_preference_t preference;
  Check(git_merge_analysis(&analysis, &preference, repo, heads, 1), "Analyze integration");
  if (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) {
    outcome.report.status = WriteStatus::NoChange;
    return outcome;
  }
  if (analysis & (GIT_MERGE_ANALYSIS_FASTFORWARD | GIT_MERGE_ANALYSIS_UNBORN)) {
    const auto head = expected.value_or(ReadHead(repo));
    if (expected) CheckHead(repo, head);
    SafeFastForward(repo, head, commit.get());
    outcome.commit = target;
    return outcome;
  }
  if (mode == PullMode::FastForwardOnly) {
    Fail(ErrorCode::NonFastForward, "Upstream diverged from the current branch");
  }
  RequireClean(repo);
  RequireUnsignedCommitPolicy(repo);
  auto target_tree = CommitTree(commit.get());
  RequireCheckoutFilters(repo, target_tree.get());
  if (mode == PullMode::Rebase) {
    RequireNoClientHooks(repo, {"pre-rebase", "post-rewrite"});
    if (expected) CheckHead(repo, *expected);
    BeginRebase(repo, target, outcome, stop);
    return outcome;
  }
  RequireNoClientHooks(repo, {"pre-merge-commit", "post-merge", "commit-msg"});
  auto checkout = CheckoutOptions();
  if (expected) CheckHead(repo, *expected);
  CheckCheckout(git_merge(repo, heads, 1, nullptr, &checkout), "Merge");
  PopulateOperation(repo, outcome);
  if (outcome.report.status != WriteStatus::NeedsResolution) {
    CommitIntegration(repo, {}, outcome, expected);
  }
  return outcome;
}

} // namespace huxerui::git::detail

namespace huxerui::git {
using namespace detail;

Task<GitResult<WorktreeVersion>> Repository::ReadWorktreeVersionAsync() const {
  return Query(state_, [](auto* repo, std::stop_token) { return ReadWorktreeVersion(repo); });
}

Task<ChangeOutcome> Repository::SwitchAsync(std::string target, HeadState expected, bool allow_overwrite,
                                           std::optional<WorktreeVersion> worktree) const {
  return Write<ChangeOutcome>(state_, [target = std::move(target), expected = std::move(expected), allow_overwrite,
                                      worktree = std::move(worktree)]
                                      (auto* repo, auto& outcome, std::stop_token) {
    RequireIdle(repo);
    CheckHead(repo, expected);
    if (allow_overwrite) {
      CheckWorktreeVersion(repo, worktree);
    }
    auto object = Revision(repo, target);
    auto commit = Make<git_object, git_object_free>("Resolve switch target", git_object_peel, object.get(),
                                                     GIT_OBJECT_COMMIT);
    auto tree = CommitTree(reinterpret_cast<git_commit*>(commit.get()));
    RequireCheckoutFilters(repo, tree.get());
    Ref branch(nullptr, git_reference_free);
    const auto short_name = target.starts_with("refs/heads/") ? target.substr(11) : target;
    git_reference* reference = nullptr;
    const int branch_code = git_branch_lookup(&reference, repo, short_name.c_str(), GIT_BRANCH_LOCAL);
    branch.reset(reference);
    if (branch_code != GIT_ENOTFOUND && branch_code != GIT_EINVALIDSPEC) {
      Check(branch_code, "Find local branch");
    }
    if (branch && expected.reference == git_reference_name(branch.get())) {
      outcome.report.status = WriteStatus::NoChange;
      return;
    }
    if (branch && git_branch_is_checked_out(branch.get()) == 1) {
      Fail(ErrorCode::Busy, "The branch is checked out in another worktree");
    }
    auto options = CheckoutOptions(allow_overwrite);
    outcome.worktree = outcome.index = WriteEffect::Unknown;
    CheckCheckout(git_checkout_tree(repo, commit.get(), &options), "Switch branch");
    outcome.worktree = outcome.index = WriteEffect::Changed;
    CheckHead(repo, expected);
    Check(branch ? git_repository_set_head(repo, git_reference_name(branch.get()))
                 : git_repository_set_head_detached(repo, git_object_id(commit.get())), "Set switched HEAD");
    outcome.references = WriteEffect::Changed;
  });
}

Task<ChangeOutcome> Repository::RestoreAsync(RestoreRequest request) const {
  return Write<ChangeOutcome>(state_, [request = std::move(request)](auto* repo, auto& outcome, std::stop_token stop) mutable {
    RequireIdle(repo);
    if (request.allow_overwrite) {
      CheckWorktreeVersion(repo, request.expected_worktree);
    }
    if (request.paths.empty() || (request.source == RestoreSource::Index && request.target != RestoreTarget::Worktree)) {
      Fail(ErrorCode::InvalidArgument, "Restore requires explicit paths and a valid source and target");
    }
    std::vector<char*> paths;
    for (auto& path : request.paths) {
      ValidatePath(path);
      paths.push_back(path.data());
    }
    git_strarray pathspec{paths.data(), paths.size()};
    auto tree = request.source == RestoreSource::Head ? HeadTree(repo) : Tree(nullptr, git_tree_free);
    RequireCheckoutFilters(repo, tree.get(), request.paths);
    if (request.target == RestoreTarget::Index) {
      RestoreIndexPaths(repo, tree.get(), pathspec, stop, outcome);
      if (outcome.report.status == WriteStatus::Completed) {
        outcome.paths = std::move(request.paths);
      }
      return;
    }

    auto options = CheckoutOptions(request.allow_overwrite);
    options.paths = pathspec;
    options.checkout_strategy |= GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;
    if (request.target != RestoreTarget::Both) {
      options.checkout_strategy |= GIT_CHECKOUT_DONT_UPDATE_INDEX;
    }
    if (request.source == RestoreSource::Head && !tree) {
      Fail(ErrorCode::NotFound, "There is no HEAD tree to restore");
    }
    outcome.worktree = WriteEffect::Unknown;
    CheckCheckout(tree ? git_checkout_tree(repo, reinterpret_cast<git_object*>(tree.get()), &options)
                       : git_checkout_index(repo, nullptr, &options), "Restore working directory paths");
    outcome.worktree = WriteEffect::Changed;
    if (request.target == RestoreTarget::Both) {
      outcome.index = WriteEffect::Changed;
    }
    outcome.paths = std::move(request.paths);
  });
}

Task<ChangeOutcome> Repository::ResetAsync(ObjectId id, ResetMode mode, HeadState expected,
                                           bool allow_overwrite, std::optional<WorktreeVersion> worktree) const {
  return Write<ChangeOutcome>(state_, [id = std::move(id), mode, expected = std::move(expected), allow_overwrite,
                                      worktree = std::move(worktree)]
      (auto* repo, auto& outcome, std::stop_token) {
    RequireIdle(repo);
    CheckHead(repo, expected);
    if (mode == ResetMode::Hard && !allow_overwrite) {
      Fail(ErrorCode::LocalChangesWouldBeOverwritten, "Hard reset requires explicit confirmation");
    }
    auto commit = FindCommit(repo, id);
    if (mode == ResetMode::Hard) {
      CheckWorktreeVersion(repo, worktree);
      RequireCheckoutFilters(repo, CommitTree(commit.get()).get());
      outcome.worktree = WriteEffect::Unknown;
    }
    if (mode != ResetMode::Soft) {
      outcome.index = WriteEffect::Unknown;
    }
    outcome.references = WriteEffect::Unknown;
    const auto reset = mode == ResetMode::Soft ? GIT_RESET_SOFT :
                      mode == ResetMode::Mixed ? GIT_RESET_MIXED : GIT_RESET_HARD;
    Check(git_reset(repo, reinterpret_cast<git_object*>(commit.get()), reset, nullptr), "Reset repository");
    outcome.references = WriteEffect::Changed;
    if (mode != ResetMode::Soft) outcome.index = WriteEffect::Changed;
    if (mode == ResetMode::Hard) outcome.worktree = WriteEffect::Changed;
  });
}

Task<IntegrationOutcome> Repository::MergeAsync(ObjectId target) const {
  return Write<IntegrationOutcome>(state_, [target = std::move(target)](auto* repo, auto& outcome, auto stop) {
    outcome = Integrate(repo, target, PullMode::Merge, stop);
  });
}

Task<IntegrationOutcome> Repository::MergeAsync(ObjectId target, HeadState expected) const {
  return Write<IntegrationOutcome>(state_, [target = std::move(target), expected = std::move(expected)]
      (auto* repo, auto& outcome, auto stop) {
    outcome = Integrate(repo, target, PullMode::Merge, stop, expected);
  });
}

static void StartLinearRebase(git_repository* repo, const ObjectId& target, IntegrationOutcome& outcome,
                              std::stop_token stop, const std::optional<HeadState>& expected = {}) {
  RequireIdle(repo);
  if (expected) CheckHead(repo, *expected);
  RequireClean(repo);
  RequireUnsignedCommitPolicy(repo);
  RequireNoClientHooks(repo, {"pre-rebase", "post-rewrite"});
  auto target_commit = FindCommit(repo, target);
  RequireCheckoutFilters(repo, CommitTree(target_commit.get()).get());
  if (expected) CheckHead(repo, *expected);
  BeginRebase(repo, target, outcome, stop);
}

Task<IntegrationOutcome> Repository::RebaseAsync(ObjectId target) const {
  return Write<IntegrationOutcome>(state_, [target = std::move(target)](auto* repo, auto& outcome, auto stop) {
    StartLinearRebase(repo, target, outcome, stop);
  });
}

Task<IntegrationOutcome> Repository::RebaseAsync(ObjectId target, HeadState expected) const {
  return Write<IntegrationOutcome>(state_, [target = std::move(target), expected = std::move(expected)]
      (auto* repo, auto& outcome, auto stop) {
    StartLinearRebase(repo, target, outcome, stop, expected);
  });
}

Task<IntegrationOutcome> Repository::CherryPickAsync(ObjectId target, unsigned mainline) const {
  return Write<IntegrationOutcome>(state_, [target = std::move(target), mainline](auto* repo, auto& outcome,
                                                                                std::stop_token) {
    RequireIdle(repo);
    RequireClean(repo);
    auto commit = FindCommit(repo, target);
    RequireCheckoutFilters(repo, CommitTree(commit.get()).get());
    git_cherrypick_options options = GIT_CHERRYPICK_OPTIONS_INIT;
    options.mainline = mainline;
    options.checkout_opts = CheckoutOptions();
    Check(git_cherrypick(repo, commit.get(), &options), "Cherry-pick commit");
    PopulateOperation(repo, outcome);
    if (outcome.report.status != WriteStatus::NeedsResolution) {
      CommitIntegration(repo, {}, outcome);
    }
  });
}

Task<IntegrationOutcome> Repository::RevertAsync(ObjectId target, unsigned mainline) const {
  return Write<IntegrationOutcome>(state_, [target = std::move(target), mainline](auto* repo, auto& outcome,
                                                                                std::stop_token) {
    RequireIdle(repo);
    RequireClean(repo);
    auto commit = FindCommit(repo, target);
    git_revert_options options = GIT_REVERT_OPTIONS_INIT;
    options.mainline = mainline;
    options.checkout_opts = CheckoutOptions();
    Check(git_revert(repo, commit.get(), &options), "Revert commit");
    PopulateOperation(repo, outcome);
    if (outcome.report.status != WriteStatus::NeedsResolution) {
      CommitIntegration(repo, {}, outcome);
    }
  });
}

Task<GitResult<std::optional<OperationSnapshot>>> Repository::ReadOperationAsync() const {
  return Query(state_, [](auto* repo, std::stop_token) { return ReadOperation(repo); });
}

Task<ChangeOutcome> Repository::ResolveConflictAsync(ResolveRequest request) const {
  return Write<ChangeOutcome>(state_, [request = std::move(request)](auto* repo, auto& outcome, std::stop_token) {
    IndexEdit index_lock(repo);
    const auto operation = RequireOperation(repo, request.expected);
    const auto path = LocalRelativePath(repo, request.path);
    const auto item = std::ranges::find(operation.conflicts, path, &ConflictEntry::path);
    if (item == operation.conflicts.end()) {
      Fail(ErrorCode::StaleSnapshot, "The conflict is no longer present");
    }
    RequireNoExternalFilters(repo, path);
    auto* index = index_lock.Edit();
    const auto selected = request.resolution == Resolution::Ours ? item->ours :
                          request.resolution == Resolution::Theirs ? item->theirs : std::nullopt;
    const bool delete_path = request.resolution == Resolution::Remove ||
        ((request.resolution == Resolution::Ours || request.resolution == Resolution::Theirs) && !selected);
    if (delete_path) {
      std::error_code error;
      fs::remove(Utf8Path(git_repository_workdir(repo)) / Utf8Path(path), error);
      if (error) {
        Fail(ErrorCode::Io, "Remove conflict path: " + error.message());
      }
      Check(git_index_conflict_remove(index, path.c_str()), "Remove conflict stages");
      outcome.worktree = WriteEffect::Changed;
    } else if (request.resolution == Resolution::Worktree) {
      Check(git_index_add_bypath(index, path.c_str()), "Stage the edited resolution");
    } else {
      git_index_entry entry{};
      entry.path = path.c_str();
      if (selected) {
        if (selected->path != path || (selected->mode != GIT_FILEMODE_BLOB &&
                                      selected->mode != GIT_FILEMODE_BLOB_EXECUTABLE)) {
          Fail(ErrorCode::Unsupported, "Edit this path explicitly to resolve a rename or special file conflict");
        }
        entry.id = Parse(selected->id);
        entry.mode = selected->mode;
        Check(git_index_add(index, &entry), "Stage the selected side");
      } else {
        entry.mode = item->ours && item->ours->mode == GIT_FILEMODE_BLOB_EXECUTABLE
                       ? GIT_FILEMODE_BLOB_EXECUTABLE : GIT_FILEMODE_BLOB;
        Check(git_index_add_from_buffer(index, &entry, request.content.data(), request.content.size()),
              "Stage the resolved contents");
      }
      const int removed = git_index_conflict_remove(index, path.c_str());
      if (removed != GIT_ENOTFOUND) {
        Check(removed, "Clear resolved conflict stages");
      }
      auto options = CheckoutOptions(true);
      auto* name = const_cast<char*>(path.c_str());
      options.paths = {&name, 1};
      options.checkout_strategy |= GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH | GIT_CHECKOUT_DONT_UPDATE_INDEX;
      outcome.worktree = WriteEffect::Unknown;
      Check(git_checkout_index(repo, index, &options), "Write conflict resolution to the working directory");
      outcome.worktree = WriteEffect::Changed;
    }
    index_lock.Publish();
    outcome.index = WriteEffect::Changed;
    outcome.paths.push_back(path);
  });
}

Task<IntegrationOutcome> Repository::ContinueAsync(OperationVersion expected, std::optional<std::string> message) const {
  return Write<IntegrationOutcome>(state_, [expected = std::move(expected), message = std::move(message)]
      (auto* repo, auto& outcome, auto stop) {
    const auto operation = RequireOperation(repo, expected);
    if (!operation.conflicts.empty()) {
      outcome.report.status = WriteStatus::NeedsResolution;
      outcome.operation = operation;
      return;
    }
    if (!operation.can_continue) {
      Fail(ErrorCode::Unsupported, "This Git operation requires an external continuation");
    }
    if (operation.kind == OperationKind::Rebase) {
      auto rebase = OpenRebase(repo);
      Replay(repo, rebase.get(), outcome, stop, operation.step != 0, message);
    } else {
      CommitIntegration(repo, message, outcome);
    }
  });
}

Task<IntegrationOutcome> Repository::SkipAsync(OperationVersion expected) const {
  return Write<IntegrationOutcome>(state_, [expected = std::move(expected)](auto* repo, auto& outcome, auto stop) {
    const auto operation = RequireOperation(repo, expected);
    if (!operation.can_skip) {
      Fail(ErrorCode::Unsupported, "The current operation does not support skipping");
    }
    auto rebase = OpenRebase(repo);
    auto head = ReadHead(repo);
    auto current = FindCommit(repo, *head.target);
    auto tree = CommitTree(current.get());
    auto options = CheckoutOptions(true);
    Check(git_checkout_tree(repo, reinterpret_cast<git_object*>(tree.get()), &options), "Discard current rebase step");
    Replay(repo, rebase.get(), outcome, stop, false);
  });
}

Task<ChangeOutcome> Repository::AbortAsync(OperationVersion expected, bool allow_overwrite) const {
  return Write<ChangeOutcome>(state_, [expected = std::move(expected), allow_overwrite]
      (auto* repo, auto& outcome, std::stop_token) {
    const auto operation = RequireOperation(repo, expected);
    if (!operation.can_abort) {
      Fail(ErrorCode::Unsupported, "This Git operation requires an external abort");
    }
    if (!allow_overwrite) {
      RequireClean(repo);
    }
    outcome.worktree = outcome.index = outcome.references = WriteEffect::Unknown;
    if (operation.kind == OperationKind::Rebase) {
      auto rebase = OpenRebase(repo);
      Check(git_rebase_abort(rebase.get()), "Abort rebase");
    } else {
      auto original = FindCommit(repo, *operation.original_head);
      Check(git_reset(repo, reinterpret_cast<git_object*>(original.get()), GIT_RESET_HARD, nullptr),
            "Restore the original integration state");
      Check(git_repository_state_cleanup(repo), "Clean aborted operation state");
    }
    outcome.worktree = outcome.index = outcome.references = WriteEffect::Changed;
  });
}

} // namespace huxerui::git
