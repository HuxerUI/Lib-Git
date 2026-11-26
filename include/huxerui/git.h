#pragma once

/// @file
/// @brief Repository inspection, guarded writes, and remote transfers for HuxerUI.
///
/// Include <huxerui/git.h> and link HuxerUI::Git. Register Install() as an application hook when resolving
/// GitService through the HuxerUI service table.
///
/// Repository Tasks are lazy. Run them on the owning application thread through a live TaskScope.
/// Git work runs on library workers and completion resumes the awaiting Task on its application thread.
/// Task cancellation suppresses delivery and may stop work cooperatively. It cannot undo Git writes.
/// A Repository opened through FileReference retains the access grant needed by its jobs.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/data.h>
#include <huxerui/file.h>
#include <huxerui/task.h>

namespace huxerui {
class ApplicationContext;
}

namespace huxerui::git {

namespace detail {
struct RepositoryState;
struct StageState;
struct VersionState;
struct HistoryState;
struct Observer;
template <class Outcome> struct TransferState;
}

/// Portable categories for repository, storage, and transport errors.
enum class ErrorCode {
  InvalidArgument, NotFound, AlreadyExists, Unsupported, PermissionDenied, Busy, StaleSnapshot,
  LocalChangesWouldBeOverwritten, Conflict, NoUpstream, NonFastForward, AuthenticationRequired,
  TrustFailed, Cancelled, Network, Io, Git,
};

/// A stable category and diagnostic. The libgit2 code is optional and may vary with the backend.
struct GitError {
  /// Portable category for branching on a failure.
  ErrorCode code = ErrorCode::Git;
  /// Diagnostic for display or logging. Its wording is not a stable identifier.
  std::string message;
  /// Original libgit2 error number when available. Zero means no libgit2 code was recorded.
  int libgit2_code = 0;
};

/// A value-or-error result for queries, handle acquisition, and request admission.
/// Check Succeeded() before using Value() or Error().
/// Write outcomes carry their WriteReport and observed effects together.
/// @tparam T The success payload, or void for a result without a payload.
template <class T> using GitResult = Result<T, GitError>;

/// Git implementation selected for an operation.
enum class Backend { Libgit2, GitCli };

enum class WriteStatus {
  Completed,
  NoChange,
  /// A precondition, request, or Git safety policy prevented the requested action.
  Rejected,
  /// A persisted operation needs conflict resolution or an explicit continuation.
  NeedsResolution,
  /// A cooperative stop was observed. Inspect the outcome for writes that already happened.
  Stopped,
  Failed,
};

enum class WriteEffect {
  Unchanged,
  Changed,
  /// The command did not establish the final state. Inspect the affected resource before retrying.
  Unknown,
};

/// Reports the command's disposition alongside any issue. Failed or stopped writes can have observable effects.
struct WriteReport {
  WriteStatus status = WriteStatus::Completed;
  /// Diagnostic for a failure or a completed operation that still needs review.
  std::optional<GitError> issue;
  /// Selected implementation. A request rejected before backend selection can retain the default.
  Backend backend = Backend::Libgit2;

  /// @return True when the operation completed or required no changes.
  bool Succeeded() const noexcept { return status == WriteStatus::Completed || status == WriteStatus::NoChange; }
};

enum class HashAlgorithm { Sha1, Sha256 };

/// A full Git object name. This distribution currently reads SHA-1 repositories.
struct ObjectId {
  std::string hex;
  HashAlgorithm algorithm = HashAlgorithm::Sha1;
  bool operator==(const ObjectId&) const = default;
};

enum class HeadKind { Attached, Detached, Unborn };

/// The HEAD attachment and current target. An unborn HEAD has a branch name and no target.
struct HeadState {
  HeadKind kind = HeadKind::Unborn;
  /// Full branch ref for attached and unborn HEAD. The literal name HEAD for detached HEAD.
  std::string reference;
  /// Current commit ID when HEAD resolves to a commit.
  std::optional<ObjectId> target;
  bool operator==(const HeadState&) const = default;
};

/// Repository location and access returned by DiscoverAsync().
struct RepositoryLocation {
  File git_directory;
  /// Working directory, absent for a bare repository.
  std::optional<File> work_directory;
  /// Directory grant retained when discovery used a FileReference.
  std::optional<FileReference> access;
};

/// Application-accessible path or directory grant. The current repository backend needs a local filesystem path.
using RepositorySource = std::variant<File, FileReference>;

/// Opaque version of the HEAD/index pair read by StatusAsync() or a staged diff summary.
class CommitPrecondition {
public:
  CommitPrecondition() = default;

  /// @return True when the token contains a review. A valid token can still be stale.
  bool IsValid() const noexcept { return static_cast<bool>(state_); }

private:
  explicit CommitPrecondition(std::shared_ptr<const detail::VersionState> state) : state_(std::move(state)) {}
  std::shared_ptr<const detail::VersionState> state_;
  friend class Repository;
};

/// The index changes for a path and the independent changes in the working directory.
struct FileStatus {
  /// Current repository-relative path.
  std::string path;
  /// Previous path reported by the comparison. For an ordinary modification it may equal path.
  std::string previous_path;
  /// HEAD-to-index changes.
  bool staged = false;
  /// Index-to-worktree changes.
  bool unstaged = false;
  bool untracked = false;
  bool ignored = false;
  bool conflicted = false;
  bool deleted = false;
};

/// Controls whether status includes worktree paths outside the index.
struct StatusQuery {
  bool include_untracked = true;
  bool include_ignored = false;
};

/// Worktree status and a HEAD/index token that can be carried into a reviewed commit request.
struct StatusSnapshot {
  HeadState head;
  CommitPrecondition commit;
  std::vector<FileStatus> files;
};

/// A worktree review identifier. Destructive commands require a fresh review of the affected repository state.
struct WorktreeVersion {
  /// Opaque token from ReadWorktreeVersionAsync(). An empty or stale value cannot authorize an overwrite.
  std::string value;
  bool operator==(const WorktreeVersion&) const = default;
};

enum class OperationKind { Merge, Rebase, CherryPick, Revert, Other };

/// Repository details usable without scanning the worktree or traversing history.
struct RepositorySnapshot {
  File git_directory;
  std::optional<File> work_directory;
  HeadState head;
  /// Full tracking ref for an attached HEAD, when configured.
  std::optional<std::string> upstream;
  /// Persisted operation kind, if any. Use ReadOperationAsync() for the current conflicts and version.
  std::optional<OperationKind> operation;
  bool shallow = false;
};

/// Author or committer identity. A missing timestamp uses the current local time.
struct Signature {
  std::string name;
  std::string email;
  std::optional<std::chrono::sys_seconds> time;
  /// Offset from UTC in minutes when an explicit timestamp is supplied.
  int time_offset_minutes = 0;
};

/// Metadata for a commit in a pinned history walk. Parent order follows the commit object.
struct CommitSummary {
  ObjectId id;
  std::vector<ObjectId> parents;
  std::string subject;
  Signature author;
  std::chrono::sys_seconds committed_at{};
};

/// A commit with its decoded message, root tree, and committer.
struct CommitDetails {
  CommitSummary summary;
  ObjectId tree;
  std::string message;
  Signature committer;
};

/// Selects pinned starting commits for a history walk.
struct HistoryQuery {
  /// Explicit tips. An empty list includes HEAD if it resolves.
  std::vector<ObjectId> starts;
  /// Also use the local branches, remote-tracking branches, and tag refs as starting points.
  bool all_refs = false;
};

/// A page of commit metadata. An exhausted page can still contain the final commits.
struct HistoryPage {
  std::vector<CommitSummary> commits;
  bool exhausted = false;
  bool shallow = false;
};

/// Sequential traversal with independent storage. Cancellation preserves an undelivered page for the next read.
class HistoryCursor {
public:
  HistoryCursor(HistoryCursor&&) noexcept = default;

  HistoryCursor& operator=(HistoryCursor&&) noexcept = default;

  HistoryCursor(const HistoryCursor&) = delete;

  HistoryCursor& operator=(const HistoryCursor&) = delete;

  /// @brief Reads the next page from the pinned history.
  /// @param limit Maximum commits to return, in [1, 4096].
  /// @return A Task yielding a page. An overlapping read on this cursor returns Busy.
  /// Canceling the Task preserves any undelivered commits for the next read.
  Task<GitResult<HistoryPage>> NextAsync(std::size_t limit = 128);

private:
  explicit HistoryCursor(std::shared_ptr<detail::HistoryState> state) : state_(std::move(state)) {}
  std::shared_ptr<detail::HistoryState> state_;
  friend class Repository;
};

enum class DiffKind { Worktree, Staged, Commit };
enum class DeltaKind { Added, Deleted, Modified, Renamed, Copied, TypeChanged, Untracked, Conflicted, Other };
enum class LineKind { Context, Added, Deleted, Marker };

/// A worktree, staged, or committed comparison. Commit comparisons use an optional explicit base or parent index.
struct DiffQuery {
  /// Worktree compares the index with working files. Staged compares HEAD with the index.
  DiffKind kind = DiffKind::Worktree;
  /// Target commit, required for a committed comparison.
  std::optional<ObjectId> commit;
  /// Optional explicit base commit. Takes precedence over parent_index.
  std::optional<ObjectId> base;
  /// Zero-based parent of commit. A root commit at index zero is compared with an empty tree.
  std::size_t parent_index = 0;
  /// Literal repository-relative path filter. An empty list compares all paths.
  std::vector<std::string> paths;
  /// Ask Git to match rename candidates in the comparison.
  bool detect_renames = false;
};

/// A file-level comparison. One side can be absent for additions or deletions.
struct FileDelta {
  std::string old_path;
  std::string new_path;
  DeltaKind kind = DeltaKind::Other;
  std::uint32_t old_mode = 0;
  std::uint32_t new_mode = 0;
  std::optional<ObjectId> old_id;
  std::optional<ObjectId> new_id;
  bool binary = false;
};

/// Changed paths and, for a staged comparison, the precondition used to commit that exact review.
struct DiffSummary {
  std::vector<FileDelta> files;
  /// Present for a staged comparison against the observed HEAD.
  CommitPrecondition commit;
};

/// A patch line with its original bytes and one-based source line numbers. Missing line numbers are -1.
struct DiffLine {
  /// Selection ID scoped to the file diff. Only added and deleted lines may be selected.
  std::uint32_t id = 0;
  LineKind kind = LineKind::Context;
  int old_line = -1;
  int new_line = -1;
  std::string text;
};

/// A contiguous patch range. Its ID is meaningful only within the returned file diff.
struct DiffHunk {
  std::uint32_t id = 0;
  std::string header;
  std::vector<DiffLine> lines;
};

/// Opaque identity of an exact file comparison. Use only the hunk and line IDs issued with it.
class StagingSnapshot {
public:
  StagingSnapshot() = default;

  /// @return True when the patch included an eligible selection token. Applying it still rechecks its inputs.
  bool IsValid() const noexcept { return static_cast<bool>(state_); }

private:
  explicit StagingSnapshot(std::shared_ptr<const detail::StageState> state) : state_(std::move(state)) {}
  std::shared_ptr<const detail::StageState> state_;
  friend class Repository;
};

/// Select one repository-relative path and an upper bound for the textual patch.
struct PatchQuery {
  DiffQuery comparison;
  std::string path;
  /// Positive review budget in bytes, at most 32 MiB. An oversized diff can be returned as truncated.
  std::size_t max_bytes = 2 * 1024 * 1024;
};

/// A bounded review of one file. A missing staging snapshot leaves the result read-only.
struct FileDiff {
  FileDelta file;
  std::vector<DiffHunk> hunks;
  /// Valid only for an eligible, complete textual index or worktree diff.
  StagingSnapshot staging;
  /// True when the configured byte budget prevented a complete review.
  bool truncated = false;
};

enum class PatchAction { Stage, Unstage };

/// Select entire hunks or changed lines from one patch. A selection only modifies the index.
struct PatchSelectionRequest {
  /// Stage uses an index-to-worktree snapshot. Unstage uses a HEAD-to-index snapshot.
  PatchAction action = PatchAction::Stage;
  StagingSnapshot snapshot;
  /// Whole hunks identified by FileDiff::hunks.
  std::vector<std::uint32_t> hunks;
  /// Individual added or deleted lines identified by DiffLine::id.
  std::vector<std::uint32_t> lines;
};

/// A contiguous range attributed to one commit, with its original repository path.
struct BlameHunk {
  std::size_t first_line = 0;
  std::size_t line_count = 0;
  ObjectId commit;
  std::string original_path;
  Signature author;
};

/// A direct child of a tree, using Git file mode to identify blobs, trees, and gitlinks.
struct TreeEntry {
  std::string name;
  ObjectId id;
  std::uint32_t mode = 0;
};

/// A direct or symbolic reference. An annotated tag can expose its peeled object separately.
struct RefInfo {
  /// Full ref name as read from Git.
  std::string name;
  /// Direct object ID, when available.
  std::optional<ObjectId> target;
  /// Resolved target when the tag lookup peeled the ref successfully.
  std::optional<ObjectId> peeled;
  /// Referenced ref name for a symbolic ref. Empty for direct refs.
  std::string symbolic_target;
};

/// A local or remote-tracking branch. Checkout occupancy includes linked worktrees.
struct BranchInfo {
  std::string name;
  ObjectId target;
  std::optional<std::string> upstream;
  bool remote = false;
  bool is_head = false;
  bool checked_out = false;
};

/// Fetch and push configuration with credential-bearing URL components removed.
struct RemoteInfo {
  std::string name;
  std::string fetch_url;
  std::string push_url;
  std::vector<std::string> fetch_refspecs;
};

/// Commit counts relative to two tips. A shallow repository can limit the observed history.
struct Reachability {
  std::size_t ahead = 0;
  std::size_t behind = 0;
  bool shallow = false;
};

/// Observed effects of a write. Inspect the report and effects even when the command failed.
struct ChangeOutcome {
  WriteReport report;
  WriteEffect index = WriteEffect::Unchanged;
  WriteEffect worktree = WriteEffect::Unchanged;
  WriteEffect references = WriteEffect::Unchanged;
  std::vector<std::string> paths;
};

/// An opaque Git commit signature and the exact commit bytes covered by it.
struct CommitSignature {
  std::string signature;
  std::string signed_data;
};

/// A signing provider invoked on a Git worker. It receives the complete unsigned commit.
/// A failure leaves the branch unchanged. The library revalidates the review after this callback returns.
using CommitSigner = std::function<GitResult<std::string>(std::string_view)>;

/// Inputs for committing or amending a reviewed index.
struct CommitRequest {
  /// Token from StatusAsync() or a staged SummarizeDiffAsync() result.
  CommitPrecondition expected;
  std::string message;
  /// Override the author. Amend preserves the original author when omitted.
  std::optional<Signature> author;
  /// Override the committer. Omit to use the repository's effective identity.
  std::optional<Signature> committer;
  /// Permit a new commit whose tree matches its parent, or an empty root tree.
  bool allow_empty = false;
  /// Optional provider for the embedded commit signature.
  CommitSigner signer;
};

/// Commit creation and reference publication are reported separately.
struct CommitOutcome {
  WriteReport report;
  /// Commit object ID if created. Check head independently to determine whether it was published.
  std::optional<ObjectId> created_commit;
  WriteEffect head = WriteEffect::Unchanged;
};

enum class ResetMode { Soft, Mixed, Hard };
enum class RestoreSource { Index, Head };
enum class RestoreTarget { Worktree, Index, Both };

/// Restores explicit paths. Overwriting working files requires the reviewed WorktreeVersion.
struct RestoreRequest {
  /// Nonempty list of literal repository-relative paths.
  std::vector<std::string> paths;
  /// The index can be restored only into the worktree. HEAD can target either or both destinations.
  RestoreSource source = RestoreSource::Index;
  RestoreTarget target = RestoreTarget::Worktree;
  bool allow_overwrite = false;
  std::optional<WorktreeVersion> expected_worktree;
};

/// Identifies the exact operation observed by ReadOperationAsync(). An empty value is invalid.
struct OperationVersion {
  std::string value;
  bool operator==(const OperationVersion&) const = default;
};

/// One stage of an unresolved index entry. A missing side represents a deleted or absent path.
struct ConflictSide {
  std::string path;
  ObjectId id;
  std::uint32_t mode = 0;
};

/// The base, current-side, and incoming-side stages used to review a conflict.
struct ConflictEntry {
  std::string path;
  std::optional<ConflictSide> base;
  std::optional<ConflictSide> ours;
  std::optional<ConflictSide> theirs;
};

/// Persisted operation state and the currently available libgit2 actions.
struct OperationSnapshot {
  OperationKind kind = OperationKind::Other;
  OperationVersion version;
  std::vector<ConflictEntry> conflicts;
  /// Commit being applied or replayed, when available.
  std::optional<ObjectId> current_commit;
  /// HEAD commit before the operation began, when available.
  std::optional<ObjectId> original_head;
  std::size_t step = 0;
  std::size_t step_count = 0;
  bool can_continue = false;
  bool can_abort = false;
  bool can_skip = false;
  /// The Git executable is required to resume operations created by its interactive sequencer.
  Backend backend = Backend::Libgit2;
};

/// Explicit desktop compatibility backend. Only use it for repositories whose Git configuration is trusted.
struct GitCliOptions {
  /// Explicit path to the Git executable to run.
  File executable;
  /// Editor to run for commit messages that require interactive editing.
  std::optional<File> editor;
  /// Execute the repository's configured Git hooks as part of the requested command.
  bool run_hooks = false;
};

enum class RebaseAction { Pick, Reword, Edit, Squash, Fixup, Drop };

/// An interactive rebase entry. The plan must include every replayed commit, including explicit drops.
struct RebaseEdit {
  ObjectId commit;
  RebaseAction action = RebaseAction::Pick;
};

/// Reviewed linear history and the ordered operations to send to the selected Git executable.
struct InteractiveRebaseRequest {
  /// New base for the selected commits.
  ObjectId upstream;
  /// HEAD state from the review.
  HeadState expected;
  /// Complete plan in execution order, including any commits marked Drop.
  std::vector<RebaseEdit> edits;
};

enum class RebaseControl { Continue, Skip, Abort };
enum class LfsAction { Fetch, Pull, Push };

/// Explicit transfer using the git-lfs extension of the selected Git executable.
struct LfsRequest {
  LfsAction action = LfsAction::Fetch;
  std::string remote = "origin";
  std::vector<std::string> references;
};

enum class Resolution { Ours, Theirs, Remove, Worktree, Content };

/// Apply a resolution to one conflict under the last observed operation version.
struct ResolveRequest {
  OperationVersion expected;
  /// Conflict path from OperationSnapshot::conflicts.
  std::string path;
  Resolution resolution = Resolution::Worktree;
  /// Replacement working-file bytes when resolution is Content.
  Bytes content;
};

/// The result of applying history. An active operation is returned when integration is waiting for a resolution.
struct IntegrationOutcome {
  WriteReport report;
  std::optional<ObjectId> commit;
  std::optional<OperationSnapshot> operation;
};

/// A stash entry identified by its selected position and object ID.
struct StashInfo {
  std::size_t index = 0;
  ObjectId id;
  std::string message;
};

/// A submodule with its recorded gitlink and the current checked-out commit, when present.
struct SubmoduleInfo {
  std::string name;
  std::string path;
  std::string url;
  std::optional<ObjectId> recorded;
  std::optional<ObjectId> checked_out;
};

/// A linked worktree as observed by the repository. A missing checkout can still have administrative metadata.
struct WorktreeInfo {
  std::string name;
  File directory;
  bool locked = false;
  bool valid = false;
};

/// Features compiled into the library. Repository and remote policy may further restrict an operation.
struct Capabilities {
  bool local = false;
  bool https = false;
  bool ssh = false;
  bool shallow = false;
  bool interactive_rebase = false;
  bool signing = false;
  bool git_lfs = false;
};

/// Copyable repository handle retaining the resolved directory and any associated access grant.
/// A returned task retains its inputs and can be awaited after the originating handle is released.
class Repository {
public:
  /// @brief Reads repository metadata without scanning the worktree or traversing history.
  /// @return A Task yielding the resolved directories, HEAD, upstream, and operation kind.
  Task<GitResult<RepositorySnapshot>> InspectAsync() const;

  /// @brief Reads index and worktree changes without refreshing index stat entries on disk.
  /// @param query Controls inclusion of untracked and ignored paths.
  /// @return A Task yielding file statuses and the commit precondition for the observed HEAD and index.
  Task<GitResult<StatusSnapshot>> StatusAsync(StatusQuery query = {}) const;

  /// @brief Captures a precondition for overwriting changes in the working directory.
  /// @return A Task yielding the version to pass to SwitchAsync(), RestoreAsync(), or ResetAsync().
  Task<GitResult<WorktreeVersion>> ReadWorktreeVersionAsync() const;

  /// @brief Pins the selected starting commits for a topological history walk.
  /// @param query Explicit starting commits and whether to also include branch and tag refs.
  /// @return A Task yielding a move-only cursor. Use HistoryCursor::NextAsync() to read its pages.
  Task<GitResult<HistoryCursor>> OpenHistoryAsync(HistoryQuery query = {}) const;

  /// @brief Reads a commit without walking other revisions.
  /// @param id Full object ID of the commit.
  /// @return A Task yielding commit details, including its tree ID and ordered parents.
  Task<GitResult<CommitDetails>> ReadCommitAsync(ObjectId id) const;

  /// @brief Extracts the embedded signature and the exact signed commit bytes for a caller-supplied verifier.
  /// @param id Full object ID of the commit.
  /// @return A Task yielding the signature data. A successful empty optional means no signature was found.
  Task<GitResult<std::optional<CommitSignature>>> ReadCommitSignatureAsync(ObjectId id) const;

  /// @brief Lists the direct children of a tree object.
  /// @param id Full object ID of the tree.
  /// @return A Task yielding entry names, object IDs, and Git file modes.
  Task<GitResult<std::vector<TreeEntry>>> ReadTreeAsync(ObjectId id) const;

  /// @brief Reads the raw contents of a blob.
  /// @param id Full object ID of the blob.
  /// @param max_bytes Maximum number of bytes to return. A larger blob returns Unsupported.
  /// @return A Task yielding the complete blob as owned bytes.
  Task<GitResult<Bytes>> ReadBlobAsync(ObjectId id, std::size_t max_bytes = 8 * 1024 * 1024) const;

  /// @brief Lists changed paths without materializing every patch.
  /// @param query Comparison endpoints, optional path restriction, and rename detection policy.
  /// @return A Task yielding file deltas. Staged comparisons also return a commit precondition.
  Task<GitResult<DiffSummary>> SummarizeDiffAsync(DiffQuery query) const;

  /// @brief Builds a bounded patch for a single path.
  /// @param query Comparison, literal repository-relative path, and text budget.
  /// @return A Task yielding a file diff. Eligible index and worktree diffs carry a selection token.
  Task<GitResult<FileDiff>> ReadPatchAsync(PatchQuery query) const;

  /// @brief Attributes the requested lines in the HEAD version of a file to commits.
  /// @param path Literal repository-relative path of the file to annotate.
  /// @param first First line to include, one-based. Zero uses the start of the file.
  /// @param last Last line to include, one-based and inclusive. Zero uses the end of the file.
  /// @return A Task yielding the attribution ranges or InvalidArgument for an invalid path or reversed range.
  Task<GitResult<std::vector<BlameHunk>>> BlameAsync(std::string path, std::size_t first = 0,
                                                   std::size_t last = 0) const;

  /// @brief Stages complete files or deletions from the worktree.
  /// @param paths Literal repository-relative paths to stage.
  /// @return A Task yielding the index update outcome.
  Task<ChangeOutcome> StageFilesAsync(std::vector<std::string> paths) const;

  /// @brief Restores index entries from HEAD and preserves the working files.
  /// @param paths Literal repository-relative paths to unstage. Entries absent from HEAD are removed from the index.
  /// @return A Task yielding the index update outcome.
  Task<ChangeOutcome> UnstageFilesAsync(std::vector<std::string> paths) const;

  /// @brief Stages or unstages only the reviewed edits, without editing the working files.
  /// @param request Selection token and hunk or changed-line IDs from the same FileDiff.
  /// @return A Task yielding the index update outcome. On StaleSnapshot, read the patch again.
  Task<ChangeOutcome> ApplySelectionAsync(PatchSelectionRequest request) const;

  /// @brief Creates a commit from the reviewed index and updates the expected HEAD.
  /// @param request Review token, message, optional identities and signing provider, and empty commit policy.
  /// @return A Task reporting object creation and HEAD publication separately.
  Task<CommitOutcome> CommitAsync(CommitRequest request) const;

  /// @brief Rewrites the reviewed HEAD commit and retains its parent list.
  /// @param request Review token, replacement message, and optional identity and signature overrides.
  /// Omit the author to preserve the original author.
  /// @return A Task reporting replacement object creation and HEAD publication separately.
  Task<CommitOutcome> AmendAsync(CommitRequest request) const;

  /// @brief Switches to a local branch or detaches HEAD at another commit.
  /// @param ref_or_id Local branch name, full ref, or revision expression resolving to a commit.
  /// @param expected HEAD state observed before initiating the switch.
  /// @param allow_overwrite Whether to allow checkout to overwrite reviewed changes.
  /// @param expected_worktree Worktree version required when allow_overwrite is true.
  /// @return A Task reporting changes to the worktree, index, and refs.
  Task<ChangeOutcome> SwitchAsync(std::string ref_or_id, HeadState expected, bool allow_overwrite = false,
                                  std::optional<WorktreeVersion> expected_worktree = {}) const;

  /// @brief Restores selected paths from HEAD or the index.
  /// @param request Source, destination, literal paths, and optional authorization to overwrite worktree changes.
  /// @return A Task reporting index and worktree effects. A restore does not move HEAD.
  Task<ChangeOutcome> RestoreAsync(RestoreRequest request) const;

  /// @brief Moves HEAD to an explicit commit.
  /// @param id Full object ID of the target commit.
  /// @param mode Soft updates HEAD, Mixed also updates the index, and Hard also resets the worktree.
  /// @param expected HEAD state observed before requesting the reset.
  /// @param allow_overwrite Must be true for a hard reset.
  /// @param expected_worktree Worktree version required for a hard reset.
  /// @return A Task reporting the effects of the reset.
  Task<ChangeOutcome> ResetAsync(ObjectId id, ResetMode mode, HeadState expected, bool allow_overwrite = false,
                                 std::optional<WorktreeVersion> expected_worktree = {}) const;

  /// @brief Enumerates repository refs.
  /// @return A Task yielding refs in name order, including symbolic target names. Use ListTagsAsync() to peel tags.
  Task<GitResult<std::vector<RefInfo>>> ListRefsAsync() const;

  /// @brief Resolves a Git revision expression to its object ID.
  /// @param revision Revision expression, such as HEAD~2 or a full ref name.
  /// @return A Task yielding the resolved ID. The object need not be a commit.
  Task<GitResult<ObjectId>> ResolveRevisionAsync(std::string revision) const;

  /// @brief Counts commits exclusive to either tip.
  /// @param local Tip for the ahead count.
  /// @param upstream Tip for the behind count.
  /// @return A Task yielding both counts and a shallow-history indicator.
  Task<GitResult<Reachability>> CompareReachabilityAsync(ObjectId local, ObjectId upstream) const;

  /// @brief Reads the effective Git identity, including inherited configuration.
  /// @return A Task yielding the identity. An empty optional means there is no complete default identity.
  Task<GitResult<std::optional<Signature>>> ReadIdentityAsync() const;

  /// @brief Stores identity in this repository's configuration.
  /// @param identity Name and email to store. Timestamp and offset do not affect configuration.
  /// @return A Task yielding the configuration write outcome.
  Task<ChangeOutcome> SetIdentityAsync(Signature identity) const;

  /// @brief Lists local and remote-tracking branches.
  /// @return A Task yielding tips, upstreams, and checkout information, including linked worktrees.
  Task<GitResult<std::vector<BranchInfo>>> ListBranchesAsync() const;

  /// @brief Creates a new local branch.
  /// @param name Local branch shorthand, such as feature/topic.
  /// @param target Full object ID of the initial commit.
  /// @return A Task yielding the ref write outcome. An existing branch is preserved.
  Task<ChangeOutcome> CreateBranchAsync(std::string name, ObjectId target) const;

  /// @brief Renames a local branch after checking its observed tip.
  /// The old and new names must be able to coexist while publishing the replacement.
  /// @param name Existing local branch name or full local branch ref.
  /// @param replacement New local branch shorthand.
  /// @param expected Observed commit ID of the existing branch.
  /// @return A Task yielding the ref write outcome. If a later step fails after creating
  /// the replacement, inspect the reported effects and refresh the branch list before retrying.
  Task<ChangeOutcome> RenameBranchAsync(std::string name, std::string replacement, ObjectId expected) const;

  /// @brief Deletes a local branch that is not checked out in any worktree.
  /// @param name Local branch name or full local branch ref.
  /// @param expected Observed commit ID of the branch.
  /// @return A Task yielding the ref write outcome.
  Task<ChangeOutcome> DeleteBranchAsync(std::string name, ObjectId expected) const;

  /// @brief Changes the tracking upstream for a local branch.
  /// @param branch Local branch name or full local branch ref.
  /// @param upstream Branch shorthand, such as origin/main. An empty optional clears tracking.
  /// @return A Task yielding the configuration write outcome.
  Task<ChangeOutcome> SetUpstreamAsync(std::string branch, std::optional<std::string> upstream) const;

  /// @brief Lists local tag refs.
  /// @return A Task yielding the tags and their peeled targets when available.
  Task<GitResult<std::vector<RefInfo>>> ListTagsAsync() const;

  /// @brief Creates a lightweight or annotated tag.
  /// @param name New tag name without the refs/tags/ prefix.
  /// @param target Full ID of the object to tag.
  /// @param message Annotation to record. An empty optional creates a lightweight tag.
  /// @return A Task yielding the tag write outcome.
  Task<ChangeOutcome> CreateTagAsync(std::string name, ObjectId target, std::optional<std::string> message = {}) const;

  /// @brief Deletes a local tag after checking its observed target.
  /// @param name Tag name or full tag ref.
  /// @param expected Observed direct object ID, before peeling an annotated tag.
  /// @return A Task yielding the tag write outcome.
  Task<ChangeOutcome> DeleteTagAsync(std::string name, ObjectId expected) const;

  /// @brief Lists the remotes configured in the repository.
  /// @return A Task yielding the fetch refspecs and sanitized fetch and push URLs.
  Task<GitResult<std::vector<RemoteInfo>>> ListRemotesAsync() const;

  /// @brief Adds a named remote with its default tracking refspec.
  /// @param name New remote name.
  /// @param url Fetch URL without embedded secrets. Provide credentials through transfer authentication.
  /// @return A Task yielding the configuration write outcome.
  Task<ChangeOutcome> AddRemoteAsync(std::string name, std::string url) const;

  /// @brief Changes one URL for a configured remote.
  /// @param name Existing remote name.
  /// @param url New URL without embedded secrets.
  /// @param push_url Update the explicit push URL when true, or the fetch URL when false.
  /// @return A Task yielding the configuration write outcome.
  Task<ChangeOutcome> SetRemoteUrlAsync(std::string name, std::string url, bool push_url = false) const;

  /// @brief Renames a remote and its tracking refs.
  /// @param name Existing remote name.
  /// @param replacement New remote name.
  /// @return A Task yielding the rename outcome. A completed result can flag custom refspecs requiring review.
  Task<ChangeOutcome> RenameRemoteAsync(std::string name, std::string replacement) const;

  /// @brief Removes the local remote configuration and its tracking refs.
  /// @param name Remote to remove from this repository.
  /// @return A Task yielding the local removal outcome.
  Task<ChangeOutcome> DeleteRemoteAsync(std::string name) const;

  /// @brief Merges a commit into the current branch.
  /// @param target Full object ID of the commit to integrate.
  /// @return A Task yielding a fast-forward, merge, no-change, or conflict outcome.
  /// A conflicting outcome leaves an operation that can be opened with ReadOperationAsync().
  Task<IntegrationOutcome> MergeAsync(ObjectId target) const;

  /// @brief Merges the selected commit after checking the reviewed destination HEAD.
  /// @param target Full object ID of the commit to integrate.
  /// @param expected HEAD attachment and tip observed when the caller reviewed the destination.
  /// @return A Task reporting integration, conflicts, or a stale destination before the write starts.
  Task<IntegrationOutcome> MergeAsync(ObjectId target, HeadState expected) const;

  /// @brief Replays linear history onto the selected upstream using libgit2.
  /// @param upstream Full object ID of the new base commit.
  /// @return A Task yielding the resulting commit or an operation that needs attention.
  Task<IntegrationOutcome> RebaseAsync(ObjectId upstream) const;

  /// @brief Starts a linear rebase after checking the reviewed branch and tip.
  /// @param upstream Full object ID of the new base commit.
  /// @param expected HEAD attachment and tip observed before confirming the rebase.
  /// @return A Task reporting the resulting commit, the persisted operation, or a stale destination.
  Task<IntegrationOutcome> RebaseAsync(ObjectId upstream, HeadState expected) const;

  /// @brief Replays a typed linear rebase plan using an explicitly selected Git executable.
  /// @param request New base, expected HEAD, and complete ordered plan.
  /// @param options Git executable, editor, and hook policy. Use with trusted repository configuration.
  /// @return A Task yielding the Git CLI outcome and any current operation state.
  Task<IntegrationOutcome> InteractiveRebaseAsync(InteractiveRebaseRequest request, GitCliOptions options) const;

  /// @brief Continues, skips, or aborts an operation created by the Git CLI rebase sequencer.
  /// @param expected Version returned by ReadOperationAsync() for the current step.
  /// @param action Rebase control to execute.
  /// @param options Git executable, editor, and hook policy for this invocation.
  /// @return A Task yielding the resulting operation state or completion outcome.
  Task<IntegrationOutcome> ControlGitRebaseAsync(OperationVersion expected, RebaseControl action,
                                                   GitCliOptions options) const;

  /// @brief Applies one commit to a clean worktree and commits the result when conflict-free.
  /// @param target Full object ID of the commit to replay.
  /// @param mainline One-based parent index for a merge commit. Use zero for an ordinary commit.
  /// @return A Task yielding the new commit or a conflict operation.
  Task<IntegrationOutcome> CherryPickAsync(ObjectId target, unsigned mainline = 0) const;

  /// @brief Applies the inverse of a commit to a clean worktree and commits the result when conflict-free.
  /// @param target Full object ID of the commit to revert.
  /// @param mainline One-based parent index for a merge commit. Use zero for an ordinary commit.
  /// @return A Task yielding the new commit or a conflict operation.
  Task<IntegrationOutcome> RevertAsync(ObjectId target, unsigned mainline = 0) const;

  /// @brief Reconstructs the operation and unresolved index stages from disk.
  /// @return A Task yielding the current operation. An idle worktree returns an empty optional.
  Task<GitResult<std::optional<OperationSnapshot>>> ReadOperationAsync() const;

  /// @brief Records the resolution of one conflicted path.
  /// @param request Operation version, conflict path, and chosen resolution, including replacement bytes if supplied.
  /// @return A Task yielding index and worktree effects. A stale version is rejected.
  Task<ChangeOutcome> ResolveConflictAsync(ResolveRequest request) const;

  /// @brief Continues the observed libgit2 operation after its conflicts are resolved.
  /// @param expected Operation version from the latest ReadOperationAsync() result.
  /// @param message Optional override for the commit message at the current step.
  /// @return A Task yielding a commit, completion, or the operation that still needs resolution.
  Task<IntegrationOutcome> ContinueAsync(OperationVersion expected, std::optional<std::string> message = {}) const;

  /// @brief Discards the current libgit2 rebase step and advances the replay.
  /// @param expected Operation version that identifies the step and edits being discarded.
  /// @return A Task yielding completion or the state of the next unresolved step.
  Task<IntegrationOutcome> SkipAsync(OperationVersion expected) const;

  /// @brief Aborts the observed libgit2 operation.
  /// @param expected Operation version from the latest ReadOperationAsync() result.
  /// @param allow_overwrite Authorizes discarding the reviewed changes. Without it, the worktree must be clean.
  /// @return A Task reporting restoration of the original index, worktree, and refs.
  Task<ChangeOutcome> AbortAsync(OperationVersion expected, bool allow_overwrite = false) const;

  /// @brief Lists the current stash stack.
  /// @return A Task yielding entries with the observed position and object ID required by apply and drop.
  Task<GitResult<std::vector<StashInfo>>> ListStashesAsync() const;

  /// @brief Saves local changes in a new stash.
  /// @param message Optional stash description. An empty string lets Git supply the description.
  /// @param include_untracked Include untracked files as well as tracked changes.
  /// @return A Task yielding the stash outcome. There is no change when nothing eligible can be saved.
  Task<ChangeOutcome> SaveStashAsync(std::string message, bool include_untracked = false) const;

  /// @brief Applies a stash selected from the observed stack.
  /// @param stash Position and object ID from ListStashesAsync().
  /// @param reinstate_index Also restore the saved staged changes to the index.
  /// @param pop Remove the stash after a successful apply. A conflicting apply keeps the stash.
  /// @return A Task yielding the index, worktree, and ref effects.
  Task<ChangeOutcome> ApplyStashAsync(StashInfo stash, bool reinstate_index = false, bool pop = false) const;

  /// @brief Removes a stash after checking its observed position and object ID.
  /// @param stash Entry returned by ListStashesAsync().
  /// @return A Task yielding the drop outcome.
  Task<ChangeOutcome> DropStashAsync(StashInfo stash) const;

  /// @brief Lists the repository's registered submodules.
  /// @return A Task yielding each submodule's index and checkout commit IDs when available.
  Task<GitResult<std::vector<SubmoduleInfo>>> ListSubmodulesAsync() const;

  /// @brief Opens a checked-out submodule and retains the parent repository's directory grant.
  /// @param name Submodule name returned by ListSubmodulesAsync().
  /// @return A Task yielding a handle to the child repository.
  Task<GitResult<Repository>> OpenSubmoduleAsync(std::string name) const;

  /// @brief Registers a new submodule.
  /// @param path Repository-relative checkout path.
  /// @param url Repository URL without embedded secrets.
  /// @return A Task yielding the registration outcome. Clone and stage the submodule separately with
  /// GitService::StartCloneSubmodule() and FinalizeSubmoduleAsync().
  Task<ChangeOutcome> AddSubmoduleAsync(std::string path, std::string url) const;

  /// @brief Stages the cloned submodule's gitlink and .gitmodules entry in the parent repository.
  /// @param name Registered submodule name.
  /// @return A Task yielding the index update outcome.
  Task<ChangeOutcome> FinalizeSubmoduleAsync(std::string name) const;

  /// @brief Initializes local configuration from the registered submodule settings.
  /// @param name Registered submodule name.
  /// @return A Task yielding the configuration write outcome.
  Task<ChangeOutcome> InitSubmoduleAsync(std::string name) const;

  /// @brief Synchronizes existing local submodule configuration with .gitmodules.
  /// @param name Registered submodule name.
  /// @return A Task yielding the configuration write outcome.
  Task<ChangeOutcome> SyncSubmoduleAsync(std::string name) const;

  /// @brief Lists linked worktree records, including missing checkouts and administrative locks.
  /// @return A Task yielding the recorded worktrees.
  Task<GitResult<std::vector<WorktreeInfo>>> ListWorktreesAsync() const;

  /// @brief Creates a linked worktree.
  /// @param name Worktree name. It must be a single path component.
  /// @param directory Local directory where Git will create the checkout.
  /// @param branch Existing local branch to check out. With no branch, Git creates one named after the worktree.
  /// @return A Task yielding the worktree and ref effects.
  Task<ChangeOutcome> AddWorktreeAsync(std::string name, File directory,
                                      std::optional<std::string> branch = {}) const;

  /// @brief Sets or clears Git's administrative lock for a linked worktree.
  /// @param name Worktree name returned by ListWorktreesAsync().
  /// @param locked True to lock the record, false to unlock it.
  /// @param reason Optional explanation to record when locking.
  /// @return A Task yielding the lock update outcome.
  Task<ChangeOutcome> SetWorktreeLockedAsync(std::string name, bool locked, std::string reason = {}) const;

  /// @brief Prunes a linked worktree's administrative record.
  /// @param name Worktree name returned by ListWorktreesAsync().
  /// @param remove_directory Also remove an existing checkout when it is clean, idle, and unlocked.
  /// @return A Task yielding the prune outcome. An active or locked record is preserved by default.
  Task<ChangeOutcome> PruneWorktreeAsync(std::string name, bool remove_directory = false) const;

private:
  explicit Repository(std::shared_ptr<detail::RepositoryState> state) : state_(std::move(state)) {}
  std::shared_ptr<detail::RepositoryState> state_;
  friend class GitService;
};

enum class TransferPhase { Queued, Connecting, Receiving, Packing, Sending, UpdatingReferences,
                           CheckingOut, AwaitingAuthentication, Stopping, Finished, ResolvingCredentials };

enum class CredentialKind : unsigned { UserPassword = 1, SshKey = 2, SshAgent = 4, Username = 8, Default = 16 };

/// Transfer that is asking for a credential.
enum class CredentialOperation { Clone, Fetch, Pull, Push };

/// Current request for authentication. The endpoint has been stripped of embedded secrets.
struct AuthChallenge {
  /// Identifies this request for Transfer::AnswerAuth().
  std::uint64_t id = 0;
  std::string endpoint;
  /// Suggested username from the transport, when available.
  std::string username;
  /// Bitwise combination of the accepted CredentialKind values.
  unsigned allowed = 0;
  /// A credential offered for this authentication step was rejected.
  bool rejected = false;
};

/// An answer for the active challenge. A missing answer declines the request.
struct Credential {
  CredentialKind kind = CredentialKind::UserPassword;
  /// Explicit username. Leave empty to use the value supplied by the transport.
  std::string username;
  /// Password, token, or SSH key passphrase, according to kind.
  std::string secret;
  /// Optional public key file for SshKey.
  std::optional<File> public_key;
  /// Required private key file for SshKey. The job requires access to the file during authentication.
  std::optional<File> private_key;
};

/// Context the application uses to select an account for a remote challenge.
struct CredentialRequest {
  CredentialOperation operation = CredentialOperation::Clone;
  /// Canonical local Git directory. Absent during clone.
  std::optional<File> repository;
  /// Repository remote name. Empty during clone.
  std::string remote;
  /// Sanitized actual endpoint, suggested username, and permitted credential types.
  AuthChallenge challenge;
};

/// Looks up a credential on a Git worker after the transport attempt has released its handles.
/// Providers may run concurrently. Return promptly without accessing UI state or requesting user interaction.
/// Return an empty optional when manual authentication is needed.
using CredentialProvider = std::function<std::optional<Credential>(const CredentialRequest&)>;

/// Owned progress and the current challenge. Counters are meaningful within the current phase.
struct TransferSnapshot {
  /// Identifies the transfer across all of its snapshots.
  std::uint64_t id = 0;
  /// Increases as the snapshot changes.
  std::uint64_t revision = 0;
  TransferPhase phase = TransferPhase::Queued;
  std::size_t completed = 0;
  /// Phase-specific total. Zero may mean the transport has not reported one.
  std::size_t total = 0;
  std::size_t bytes = 0;
  std::optional<AuthChallenge> authentication;
};

/// Move-only registration. Destruction stops delivery to this observer.
class TransferSubscription {
public:
  TransferSubscription() = default;

  TransferSubscription(TransferSubscription&&) noexcept;

  TransferSubscription& operator=(TransferSubscription&&) noexcept;

  TransferSubscription(const TransferSubscription&) = delete;

  TransferSubscription& operator=(const TransferSubscription&) = delete;

  ~TransferSubscription();

private:
  explicit TransferSubscription(std::shared_ptr<detail::Observer>);
  std::shared_ptr<detail::Observer> observer_;
  template <class> friend class Transfer;
};

/// Copyable handle to an admitted background job. Waiting and stopping are independent.
/// @tparam Outcome The terminal outcome type of the command that created the transfer.
template <class Outcome> class Transfer {
public:
  /// @brief Reads the transfer's phase, counters, and any active authentication challenge.
  /// @return An owned snapshot. Use WaitAsync() for the terminal outcome.
  TransferSnapshot Snapshot() const;

  /// @brief Waits for the terminal outcome independently of other waiters.
  /// @return A Task yielding the same terminal outcome to each live waiter, including waiters added after completion.
  /// Canceling the Task detaches that wait. Use RequestStop() to signal the transfer itself.
  Task<Outcome> WaitAsync() const;

  /// @brief Observes progress on the application thread.
  /// @param scope Scope used to post delivery. A retired scope cannot receive updates.
  /// @param observer Callback receiving the current snapshot and subsequent updates. Intermediate updates may coalesce.
  /// @return A move-only registration. Keep it alive while delivery is needed.
  TransferSubscription Observe(TaskScope scope, std::function<void(TransferSnapshot)> observer) const;

  /// @brief Requests cooperative stop.
  /// @return True for the first accepted request, or false once stop was requested or the transfer finished.
  /// Use WaitAsync() to observe the outcome and any effects already committed by Git.
  bool RequestStop() const noexcept;

  /// @brief Answers the active authentication challenge.
  /// @param challenge ID from TransferSnapshot::authentication.
  /// @param credential A permitted credential for this challenge, or an empty optional to decline.
  /// @return Whether the answer was accepted. A stale or already answered challenge returns StaleSnapshot.
  /// An accepted answer does not establish that authentication or the transfer succeeded.
  GitResult<void> AnswerAuth(std::uint64_t challenge, std::optional<Credential> credential) const;

private:
  explicit Transfer(std::shared_ptr<detail::TransferState<Outcome>> state) : state_(std::move(state)) {}
  std::shared_ptr<detail::TransferState<Outcome>> state_;
  friend class GitService;
};

/// Clone into a new local directory. A depth of zero requests the complete available history.
struct CloneRequest {
  /// Repository location without embedded secrets.
  std::string url;
  /// New File path whose parent exists and is writable. This operation requires a File destination.
  RepositorySource destination;
  /// Create a repository without a working directory when true.
  bool bare = false;
  /// A positive value requests a shallow clone, subject to transport support.
  unsigned depth = 0;
  /// Branch to check out instead of the remote's default, when supplied.
  std::optional<std::string> branch;
};

/// The published clone, when ready to open, and its terminal report.
struct CloneOutcome {
  WriteReport report;
  std::optional<Repository> repository;
};

/// Named-remote fetch policy. Depth and unshallow are mutually exclusive.
struct FetchRequest {
  /// Name of a remote configured in the repository.
  std::string remote = "origin";
  /// Fetch refspecs to use in place of the configured defaults. Empty uses the configured refspecs.
  std::vector<std::string> refspecs;
  /// Prune local tracking refs removed from the remote when true.
  bool prune = false;
  /// Requested shallow depth. Zero leaves depth unspecified.
  unsigned depth = 0;
  /// Ask the transport to remove the shallow boundary.
  bool unshallow = false;
};

/// A ref update observed during fetch. An absent previous or target ID indicates creation or removal.
struct FetchedRef {
  std::string name;
  std::optional<ObjectId> previous;
  std::optional<ObjectId> target;
};

/// Result of fetching objects and updating refs. Updates already made remain visible after a later failure.
struct FetchOutcome {
  WriteReport report;
  std::vector<FetchedRef> updated_references;
  WriteEffect objects = WriteEffect::Unchanged;
};

enum class PushPolicy { Normal, ForceWithLease, Force };
enum class RefPushStatus { Updated, UpToDate, Rejected, NotAttempted, Unknown };

/// A missing source deletes the destination. For a lease, a missing expected OID means the ref must be absent.
struct PushUpdate {
  std::optional<ObjectId> source;
  /// Full ref name on the remote, such as refs/heads/main.
  std::string destination;
  PushPolicy policy = PushPolicy::Normal;
  /// Expected remote object ID for ForceWithLease. Unset means the destination must be absent.
  std::optional<ObjectId> expected;
};

/// An explicit list of remote updates. The source object is pinned by each request.
struct PushRequest {
  std::string remote = "origin";
  std::vector<PushUpdate> updates;
};

/// Result for one destination. Unknown requires checking what the remote currently advertises.
struct RefPushResult {
  std::string destination;
  RefPushStatus status = RefPushStatus::NotAttempted;
  std::string diagnostic;
};

/// Terminal push report and the individual destination results.
struct PushOutcome {
  WriteReport report;
  std::vector<RefPushResult> references;
};

enum class PullMode { FastForwardOnly, Merge, Rebase };

/// Fetch and integrate a named tracking ref with a reviewed starting HEAD.
struct PullRequest {
  FetchRequest fetch;
  /// HEAD state from inspection before the pull.
  HeadState expected;
  /// Full local tracking ref that the fetch is expected to update or advertise.
  std::string upstream_ref;
  /// Policy applied after fetch. FastForwardOnly rejects divergent histories.
  PullMode mode = PullMode::FastForwardOnly;
};

/// Completed phases remain visible even when a later phase is rejected or stopped.
struct PullOutcome {
  WriteReport report;
  std::optional<FetchOutcome> fetch;
  std::optional<IntegrationOutcome> integration;
};

/// Starts repository operations and owns admission to the blocking Git executors.
class GitService {
public:
  GitService() = default;

  /// @brief Enables automatic credential lookup for Clone, Fetch, Pull, and Push.
  /// @param provider Resolves a challenge. An empty provider preserves manual authentication.
  explicit GitService(CredentialProvider provider);

  /// @brief Reads the feature set compiled into this build.
  /// @return Capability flags independent of a repository's configuration, access grants, or remote transport.
  Capabilities GetCapabilities() const noexcept;

  /// @brief Discovers a repository starting at a directory.
  /// @param source Local directory or directory reference with an accessible local path.
  /// @param boundary Optional outer directory. A discovered repository outside it is rejected.
  /// @return A Task yielding the resolved location and any retained directory grant.
  Task<GitResult<RepositoryLocation>> DiscoverAsync(RepositorySource source, std::optional<File> boundary = {}) const;

  /// @brief Opens the specified repository without searching its parent directories.
  /// @param source Local repository directory or a directory reference exposing an accessible local path.
  /// @return A Task yielding a repository that retains any grant. Writes require write access.
  Task<GitResult<Repository>> OpenAsync(RepositorySource source) const;

  /// @brief Reopens a discovered repository.
  /// @param location Resolved Git directory and optional grant from DiscoverAsync().
  /// @return A Task yielding a repository that retains the grant.
  Task<GitResult<Repository>> OpenAsync(RepositoryLocation location) const;

  /// @brief Initializes a repository at the supplied path and preserves any existing repository there.
  /// @param source Destination path or writable directory reference exposing a local path.
  /// @param bare Create a repository without a working directory when true.
  /// @return A Task yielding a handle to the initialized repository or an error.
  Task<GitResult<Repository>> InitAsync(RepositorySource source, bool bare = false) const;

  /// @brief Queues a clone without blocking on the transfer.
  /// @param request Source URL, new local File destination, and optional branch or depth restrictions.
  /// @return A transfer handle or an admission error. The terminal outcome contains any published repository.
  GitResult<Transfer<CloneOutcome>> StartClone(CloneRequest request) const;

  /// @brief Queues a fetch from a named remote.
  /// @param repository Repository whose objects and tracking refs will be updated.
  /// @param request Remote name, refspecs, pruning policy, and shallow options.
  /// @return A transfer handle or an admission error. The terminal outcome reports observed updates.
  GitResult<Transfer<FetchOutcome>> StartFetch(Repository repository, FetchRequest request = {}) const;

  /// @brief Queues explicit ref updates for a named remote.
  /// @param repository Repository containing the objects to send and the remote configuration.
  /// @param request Destination refs, source object IDs or deletions, and push policy per ref.
  /// @return A transfer handle or an admission error. Use the terminal outcome to check each destination.
  GitResult<Transfer<PushOutcome>> StartPush(Repository repository, PushRequest request) const;

  /// @brief Queues fetch followed by the requested integration policy.
  /// @param repository Repository to fetch into and update.
  /// @param request Fetch settings, observed HEAD, tracking ref, and integration mode. Defaults to fast-forward only.
  /// @return A transfer handle or an admission error. The outcome reports fetch and integration separately.
  GitResult<Transfer<PullOutcome>> StartPull(Repository repository, PullRequest request) const;

  /// @brief Queues a transfer using the selected Git installation's Git LFS extension.
  /// @param repository Repository containing the LFS configuration and refs.
  /// @param request LFS command, remote name, and optional refs.
  /// @param options Git executable and hook policy. Use with trusted repository configuration.
  /// @return A transfer handle or an admission error. The terminal report identifies the Git CLI backend if used.
  GitResult<Transfer<ChangeOutcome>> StartLfs(Repository repository, LfsRequest request, GitCliOptions options) const;

  /// @brief Clones the repository for a registered submodule.
  /// @param repository Parent repository containing the submodule registration.
  /// @param name Registered submodule name.
  /// @return A transfer handle or an admission error. Repository::FinalizeSubmoduleAsync() stages the entry separately.
  GitResult<Transfer<ChangeOutcome>> StartCloneSubmodule(Repository repository, std::string name) const;

  /// @brief Updates submodule checkouts to their recorded index gitlinks.
  /// @param repository Parent repository containing the gitlinks.
  /// @param names Nonempty list of registered submodule names.
  /// @param initialize Allow initialization when a listed submodule has not been initialized.
  /// @return A transfer handle or an admission error. Completed paths remain in the terminal outcome on failure.
  GitResult<Transfer<ChangeOutcome>> StartUpdateSubmodules(Repository repository, std::vector<std::string> names,
                                                         bool initialize = true) const;

  /// @brief Reads advertised remote refs without updating local tracking refs.
  /// @param repository Repository containing the remote configuration.
  /// @param remote Configured remote name. The query uses its fetch URL.
  /// @param credential Optional credential for that endpoint. This query has no interactive challenge.
  /// @return A Task yielding the advertisement. The remote may change after the query completes.
  Task<GitResult<std::vector<RefInfo>>> ReadRemoteRefsAsync(Repository repository, std::string remote,
                                                          std::optional<Credential> credential = {}) const;

private:
  std::shared_ptr<const CredentialProvider> credentials_;
};

/// @brief Registers GitService with a HuxerUI application.
/// @param context Application context supplied to the installation hook.
/// Standalone callers may construct GitService directly.
void Install(ApplicationContext& context);

} // namespace huxerui::git
