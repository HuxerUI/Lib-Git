# Lib-Git Technical Design and Feasibility Assessment

Lib-Git is feasible as a libgit2-based C++ library and a daily-use Git client for Windows, macOS, and Linux. Its first complete desktop release should cover multi-repository navigation, a commit graph, diff review, file and partial staging, remote synchronization, and conflict resolution with continue and abort. Start iOS and Android with app-owned repositories. Treat browser storage and synchronization as separate feasibility gates.

Use libgit2 for the core workflows. Offer an explicit desktop Git CLI option for full interactive rebase, Git-configured signing and hooks, external filters, credential helpers, and Git LFS. The first implementation gate is the ability to apply exactly what the user reviewed and recover the actual repository state after interruption.

## Evidence and scope

**Verified at the design baseline.** The original Lib-Git project declared `HuxerUI::Git`. The preview consumes that target through `huxerui_use_library`. The public header declares `Install(ApplicationContext&)`, its implementation is empty, and the preview renders a placeholder. The installed HuxerUI SDK is 0.5.5. Its `task.h` and `file.h` match the adjacent HuxerUI source headers. The comparable library layouts are Lib-CSS, Lib-AVKit, and Lib-RichEditor. Local references are listed under Repository references.

**Verified upstream.** API and implementation claims use the libgit2 v1.9.7 tag. The relevant findings include its rebase operation handling, topology precomputation, index lock ordering, depth restrictions on the local transport, and Smart push negotiation. [libgit2 releases](https://github.com/libgit2/libgit2/releases) · [v1.9.7 public headers](https://github.com/libgit2/libgit2/tree/v1.9.7/include/git2)

**Proposed design and prototype gates.** The API, scheduler, validation strategy, platform support levels, and client workflows below are proposed. Correctness under cross-process races, performance, transport cancellation, provider access, and package delivery require runtime acceptance tests. See the [platform support matrix](../README.md#platform-support) for current capabilities.

The implementation took the controlled index-publication path described below. `ReadPatchAsync` and `ApplySelectionAsync` bind the review to the original path, index entry, and worktree bytes. The writer acquires Git's index lock, prepares the update using the current on-disk index, and publishes the new index. The integration tests compare staged and committed content against the Git executable and exercise a worktree edit and an independent index write from outside the library. [Index publication](../src/git.cpp) · [Selection application](../src/changes.cpp) · [Repository tests](../tests/repository.cpp)

## Capability matrix

**Direct** means libgit2 has an appropriate primitive. **Compose** means Lib-Git owns orchestration, safety, or application-facing semantics. **Dependency** means an external component or backend supplies part of the capability. **Deferred** means detect the feature and report the support limit.

### Repository and inspection

| Capability | Coverage | Contract |
| --- | --- | --- |
| Discover, initialize, open | Direct + compose | Discover within an explicit boundary and open ordinary, bare, and linked-worktree repositories. Distinguish detached and unborn HEAD. Preserve ownership and access checks. [Repository API](https://libgit2.org/docs/reference/v1.9.0/repository/index.html) |
| Clone | Direct + compose | Support ordinary and bare clone, progress, authentication, and destination ownership. Recursive submodules, shallow history, and LFS have separate capabilities. [Clone implementation](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/clone.c) |
| HEAD, refs, branches | Direct + compose | Resolve and enumerate refs, create, rename and delete branches, set upstream, compute ahead/behind and inspect reflogs. Check expected ref targets and branch occupancy across worktrees. [Refs API](https://libgit2.org/docs/reference/v1.9.0/refs/index.html) · [Git worktree](https://git-scm.com/docs/git-worktree) |
| Tags and remote configuration | Direct + compose | Create and inspect lightweight and annotated tags. Manage names, fetch/push URLs, refspecs and tracking configuration. A remote tag deletion is a distinct push action. [Tag API](https://libgit2.org/docs/reference/v1.9.0/tag/index.html) · [Remote header](https://github.com/libgit2/libgit2/blob/v1.9.7/include/git2/remote.h) |
| History and topology | Direct + compose | Return commit IDs, parent edges, ref decorations and a cursor grounded in fixed starting OIDs. The app lays out lanes. Expose shallow boundaries or missing objects. [Revwalk API](https://libgit2.org/docs/reference/main/revwalk/index.html) |
| Worktree, index, status | Direct + compose | Report HEAD-to-index and index-to-worktree changes independently. Represent untracked and ignored files, modes, symlinks, gitlinks and conflicts. Run status with index writes disabled. [Index API](https://libgit2.org/docs/reference/main/index/index.html) · [Status options](https://github.com/libgit2/libgit2/blob/v1.9.7/include/git2/status.h) |
| Diff, patch, blame | Direct + compose | Support tree/index/worktree comparisons, comparisons between commits, merge-commit diffs against a selected parent, rename detection, and binary classification. Standard blame can be scoped to a file or line range. Several copy and move tracking flags remain unimplemented in this baseline. [Diff API](https://libgit2.org/docs/reference/main/diff/index.html) · [Blame header](https://github.com/libgit2/libgit2/blob/v1.9.7/include/git2/blame.h) |

### Local changes and history operations

| Capability | Coverage | Contract |
| --- | --- | --- |
| Stage and unstage files | Direct + compose | Stage a worktree version or deletion. Unstage to the HEAD version in the index, or remove the entry for an unborn HEAD. Handle both paths of a rename. [Index API](https://libgit2.org/docs/reference/main/index/index.html) · [Git restore](https://git-scm.com/docs/git-restore) |
| Stage and unstage hunks or lines | Compose | Use apply, blob and index primitives with snapshot validation. Line selection constructs the selected edits. For unstage, reverse the selected HEAD-to-index edits against the verified index. Partial additions/deletions need explicit result blobs or entry removal. Binary files, symlinks, gitlinks, unsupported filters, and mode changes use eligible whole-file actions or a capability error. [Apply API](https://libgit2.org/docs/reference/main/apply/index.html) · [Apply implementation](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/apply.c) |
| Commit and amend | Direct + compose | Commit the selected index state with explicit identity and an expected HEAD/index version. Amend can preserve or replace identity/message according to the request. A rewritten commit needs any requested signature recreated. [Commit header](https://github.com/libgit2/libgit2/blob/v1.9.7/include/git2/commit.h) |
| Signed commits and tags | Dependency | libgit2 can prepare commit bytes, attach a signature, and extract it. Signing, key access, and trust validation need a signer/verifier or the Git CLI and configured tools. Signing must be included when supported operations rewrite commits. [Commit signature API](https://libgit2.org/docs/reference/v1.9.0/commit/git_commit_create_with_signature.html) · [Git config](https://git-scm.com/docs/git-config) |
| Switch, restore, reset | Direct + compose | Combine checkout with explicit HEAD or branch updates for switch. Restore specifies source and destinations. Reset specifies soft, mixed, hard, or path scope. Check branch occupancy and local content before mutations. [Checkout semantics](https://libgit2.org/docs/reference/main/checkout/git_checkout_strategy_t.html) · [Git restore](https://git-scm.com/docs/git-restore) |
| Merge and conflict resolution | Direct + compose | Analyze fast-forward and merge cases. Apply with libgit2, read conflict stages, and manage merge commit, continuation, and abort. Begin recoverable libgit2 sessions from a known clean baseline. Repository state cleanup and worktree restoration are separate steps. [Merge API](https://libgit2.org/docs/reference/v1.9.0/merge/git_merge.html) · [Git merge](https://git-scm.com/docs/git-merge) |
| Linear rebase | Direct + compose | Use init, open, next, commit, finish, and abort. Track the current commit, conflicts, and explicit empty-commit policy. v1.9.7 generates only pick operations and skips merge commits in its replay list. [Rebase API](https://libgit2.org/docs/reference/main/rebase/index.html) · [Rebase implementation](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/rebase.c#L573) |
| Interactive rebase | Dependency | Use Git CLI for reorder, reword, edit, squash, fixup, drop, split and merge-preserving rebase, with Git-managed continuation. v1.9.7 exposes operation enum values but its open path rejects interactive and apply-based Git sessions. [Rebase header](https://github.com/libgit2/libgit2/blob/v1.9.7/include/git2/rebase.h) · [Open-state implementation](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/rebase.c#L372) · [Git rebase](https://git-scm.com/docs/git-rebase) |
| Cherry-pick and revert | Direct + compose | Apply a commit with a selected mainline for merge commits. Lib-Git owns commit, continue, abort and recovery. Multi-commit sequences can use the desktop Git sequencer until Lib-Git sequence persistence is justified. [Cherry-pick header](https://github.com/libgit2/libgit2/blob/v1.9.7/include/git2/cherrypick.h) · [Revert header](https://github.com/libgit2/libgit2/blob/v1.9.7/include/git2/revert.h) |
| Stash | Direct + compose | List, save, show, apply, pop and drop. Make untracked/ignored capture and index reinstatement explicit. Resolve a selected stash by OID and list version. Retain it after an apply conflict. [Stash API](https://libgit2.org/docs/reference/v1.9.0/stash/index.html) |
| Submodule | Direct + compose | Inspect gitlinks, add, initialize, sync, update, open and stage a gitlink. Orchestrate recursion, relative URLs, per-repository authentication and partial outcomes. Expose recorded and actual HEADs. [Submodule API](https://libgit2.org/docs/reference/main/submodule/index.html) |
| Linked worktree | Direct + compose | List, open, add, lock, unlock, validate and prune. Inspect dirty state, branch occupancy and grants before removal. Use Git CLI for move or repair where required. [Worktree API](https://libgit2.org/docs/reference/main/worktree/index.html) · [Git worktree](https://git-scm.com/docs/git-worktree) |

### Synchronization and compatibility

| Capability | Coverage | Contract |
| --- | --- | --- |
| Fetch | Direct + compose | Expose refspec, tag, prune and depth controls. Return observed ref changes and the last completed stage. Objects, shallow metadata or some refs may have changed before an error. [Remote implementation](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/remote.c) |
| Push | Direct + compose | Push explicit refspecs and report accepted, rejected or unknown status per destination ref. Treat remote deletion and non-fast-forward updates as explicit actions. Verify lease semantics per transport. [Push implementation](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/push.c) · [Git push](https://git-scm.com/docs/git-push) |
| Pull | Compose | Fetch, pin the fetched upstream OID, analyze the integration, then fast-forward, merge or rebase. Return fetch and integration outcomes separately. [Git pull](https://git-scm.com/docs/git-pull) |
| Shallow repositories | Direct + compose | Smart transport supports depth and unshallow. Local transport rejects shallow fetch. Reachability, blame, ahead/behind and push can be limited by missing history. [Release history](https://github.com/libgit2/libgit2/blob/main/docs/changelog.md#v17) · [Local transport](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/transports/local.c#L309) |
| Large repositories and special formats | Compose + deferred | Page history and load patches on demand, with memory and CPU budgets. Detect partial clone/promisor repositories, sparse checkout/index, reftable and the experimental SHA-256 build separately. Enable each combination only after compatibility testing. [Repository extensions](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/repository.c#L1864) · [Git partial clone](https://git-scm.com/docs/partial-clone) |
| Hooks, external filters and credential helpers | Dependency | libgit2 offers credential callbacks and custom filter registration. Full Git-configured hooks, external filters and helper chains require explicit desktop Git compatibility. Report server hooks through remote outcomes. [Git hooks](https://git-scm.com/docs/githooks) · [Git credentials](https://git-scm.com/docs/gitcredentials) · [Filter API](https://github.com/libgit2/libgit2/blob/v1.9.7/include/git2/sys/filter.h) |
| Git LFS | Dependency | Recognize pointers, attributes and unavailable content. Use Git CLI and git-lfs for complete clean/smudge, download, upload, pre-push and locking workflows. Guard affected operations before they store large files as ordinary blobs or push pointers without their LFS objects. [LFS specification](https://github.com/git-lfs/git-lfs/blob/main/docs/spec.md) · [LFS API](https://github.com/git-lfs/git-lfs/blob/main/docs/api/README.md) |
| Diagnostics and maintenance | Direct + compose + dependency | Expose config, reflogs, ignore/attributes, tree/blob inspection and patch import/export. Use Git CLI for full fsck, gc, maintenance and repair, with repository-aware scheduling. [Git reflog](https://git-scm.com/docs/git-reflog) · [Git pack format](https://git-scm.com/docs/gitformat-pack) |

### Operation policy

| Operation | Library default | Client decision |
| --- | --- | --- |
| Pull | Fast-forward only. Autostash and prune are off by default. Freeze the upstream OID selected from this fetch and revalidate state before integration. | Show upstream and divergence, then offer merge, rebase, or keeping the fetched state. |
| Switch, restore, reset, abort | Safe switch can carry unrelated changes. Report affected paths if data would be overwritten. Require explicit destructive action and an expected state. Keep tracked overwrite and untracked cleanup separate. | Review affected paths and the target. Confirm hard reset, untracked deletion, and an abort that would overwrite edits made during the operation. |
| Push | Ordinary push by default. Prefer a lease with the reviewed expected remote OID for a force push. Require an explicit option for unconditional force. Return per-ref outcomes. | Show remote, destination, expected OID, and rewritten range. Inspect remote state after an ambiguous response. |

Smart push can compare the expected old OID in `push_negotiation` and send the same old OID with the update on the wire. This is a candidate for lease support, subject to a server-side race test. A separate `ls-remote` followed by a forced refspec leaves a race. v1.9.7's local transport updates an existing ref without the equivalent expected-value guard. Use a validated Smart transport or Git CLI for a shared writable local remote. Treat multi-ref atomic push as a separate capability. [Push implementation](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/push.c#L382) · [Local transport implementation](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/transports/local.c#L341) · [Git wire protocol](https://git-scm.com/docs/gitprotocol-pack)

## Public C++ and coroutine API

Keep one `HuxerUI::Git` library, one application-level `GitService`, and a copyable `Repository` handle. `Install(ApplicationContext&)` can register the service with `Provide`. The public API exposes Git values and use cases. The library owns libgit2 handles through RAII and uses each mutable libgit2 handle from one execution context at a time. Add callback-scoped libgit2 handle borrowing in an opt-in header only when a concrete consumer needs it. Balance process-wide `git_libgit2_init/shutdown` after the last job and native handle have retired. [libgit2 threading](https://github.com/libgit2/libgit2/blob/v1.9.7/docs/threading.md)

### Ownership and results

A `Repository` retains its shared session. Public methods copy that session and their owned request into the task before returning. Keep these methods as ordinary functions returning a private coroutine or an `AwaitCallback` task with owned captures. A lazy coroutine that later reads the facade through `this` can outlive a temporary handle. Jobs retain their session and directory grants until cleanup completes. The same rule applies to a history cursor, a transfer handle, and a task returned from a temporary `GitService` facade. See `HuxerUI/include/huxerui/task.h:144`.

| Public value | Contract |
| --- | --- |
| `ObjectId`, `RefName`, `GitPath` | An algorithm-tagged object ID, a validated full ref name, and a repository-relative path that preserves Git's exact bytes. Use display conversion at the UI boundary. A ref can address an object or another ref. `HeadState` distinguishes attached, detached, and unborn. |
| `RepositoryLocation`, `RepositorySnapshot` | Discovery returns the resolved repository and its retained access. A snapshot exposes repository kind, HEAD, index version, upstream and in-progress operation summary. Status and expensive content are separate reads. Bare repositories report worktree capabilities as unavailable. |
| `CommitSummary`, `HistoryPage` | Object ID, ordered parents, identities, time, subject, and ref decorations from the starting snapshot. A page reports exhaustion and shallow or missing-object boundaries. One move-only cursor has one consumer and pins the starting OIDs and query. Concurrent `NextAsync()` on that cursor returns `Busy`. A new ref snapshot uses a new cursor. |
| `DiffSummary`, `FileDiff` | The summary enumerates file deltas and metadata, with a delta key for opening one patch. The patch owns paths, modes, change kind, line origins and ranges, and hunks. It reports binary or size-limit cases explicitly. A patch can expose the snapshot needed for staging only when the comparison and file are eligible. |
| `StagingSnapshot`, `IndexVersion`, `OperationVersion` | Opaque, owned preconditions with different scopes. Staging binds the two sides of the reviewed file patch. Commit binds the entire reviewed index and expected HEAD. Operation actions bind the observed session and step, including relevant conflict state. These values are refreshed from the repository and are not durable authorization tokens. |

Use the return type to express whether a call acquires a value, reports write effects, or reads an existing handle.

| Call | Return contract |
| --- | --- |
| Repository queries and handle acquisition | `Task<GitResult<T>>`. Invalid input and repository failures are returned when awaited. An optional payload keeps ordinary absence distinct from failure, such as when `ReadOperationAsync()` observes an idle worktree. `InitAsync()` reports acquisition of an initialized handle. Inspect the destination after a failed initialization if its on-disk state matters. |
| Transfer admission and authentication answers | `Start*()` returns `GitResult<Transfer<Outcome>>` for the immediate admission decision. `AnswerAuth()` returns `GitResult<void>` for the answer's acceptance. The transfer's terminal status is obtained separately. |
| Write commands and transfer completion | `Task<ChangeOutcome>`, `Task<CommitOutcome>`, `Task<IntegrationOutcome>`, or `Task<Outcome>` from `WaitAsync()`. The outcome carries status, an optional issue, and any observed effects together. A failed pull can contain a completed fetch, and a push can report different statuses for individual remote refs. |
| Direct handle operations | `GetCapabilities()`, `Snapshot()`, and `IsValid()` return values directly. `Observe()` returns its subscription. `RequestStop()` returns whether this call accepted the stop request. The terminal outcome records whether the operation stopped. |

Keep `NoChange`, `NeedsResolution`, `Stopped`, and `Failed` distinct. For each reported resource, distinguish a known change, a known lack of change, and an unknown result. Copy `git_error_last()` on the thread that observed the failure. HuxerUI task cancellation can suppress delivery independently of the command's eventual outcome. [libgit2 thread-local errors](https://github.com/libgit2/libgit2/blob/v1.9.7/docs/threading.md)

The core result and precondition shapes are below. Outcome-specific fields describe the writes each command can cause. A missing optional phase means the command never reached that phase.

```cpp
enum class Backend { Libgit2, GitCli };
enum class WriteStatus { Completed, NoChange, Rejected, NeedsResolution, Stopped, Failed };
enum class WriteEffect { Unchanged, Changed, Unknown };

struct WriteReport {
  WriteStatus status;
  std::optional<Backend> backend;
  std::optional<GitError> issue;
};

struct CommitPrecondition {
  HeadState head;
  IndexVersion index;
};

struct CommitOutcome {
  WriteReport report;
  std::optional<ObjectId> created_commit;
  WriteEffect head_effect;
};

enum class RefPushStatus { Updated, UpToDate, Rejected, NotAttempted, Unknown };

struct RefPushResult {
  RefName destination;
  RefPushStatus status;
  std::optional<GitError> issue;
};

struct PushOutcome {
  WriteReport report;
  std::vector<RefPushResult> refs;
};

struct PullOutcome {
  WriteReport report;
  std::optional<FetchOutcome> fetch;
  std::optional<IntegrationOutcome> integration;
};
```

`Rejected` is a policy or precondition outcome, such as `StaleSnapshot` or `LocalChangesWouldBeOverwritten`. An admitted pull can report a completed fetch and rejected integration. `ChangeOutcome` carries the relevant per-path or per-ref effects and refresh information. `InitOutcome` and `CloneOutcome` carry the resulting repository handle when available. A transfer's retained terminal result is an owned value that can be delivered to more than one observer.

### Repository access and local review

The declarations below show the public shape. DTOs referenced by name are specified in the tables and contracts. `RepositorySource` accepts `File` or `FileReference`, retaining the latter through discovery, open, init or clone when the location needs a grant. Discovery obeys an explicit boundary. The overload taking `RepositoryLocation` uses the resolved location and retained authorization.

```cpp
template <class T>
using GitResult = huxerui::Result<T, GitError>;

using RepositorySource = std::variant<File, FileReference>;

class GitService {
public:
  Task<GitResult<RepositoryLocation>> DiscoverAsync(RepositorySource, DiscoverOptions) const;
  Task<InitOutcome> InitAsync(RepositorySource, InitOptions) const;
  Task<GitResult<Repository>> OpenAsync(RepositorySource, OpenOptions) const;
  Task<GitResult<Repository>> OpenAsync(RepositoryLocation, OpenOptions) const;
  Task<GitResult<CapabilityReport>> CheckCloneCapabilityAsync(CloneRequest) const;
  GitResult<Transfer<CloneOutcome>> StartClone(CloneRequest) const;
};

class HistoryCursor {
public:
  HistoryCursor(HistoryCursor&&) noexcept;
  HistoryCursor& operator=(HistoryCursor&&) noexcept;
  Task<GitResult<HistoryPage>> NextAsync(PageLimit);
};

class Repository {
public:
  Task<GitResult<RepositorySnapshot>> InspectAsync() const;
  Task<GitResult<StatusSnapshot>> StatusAsync(StatusQuery = {}) const;
  Task<GitResult<HistoryCursor>> OpenHistoryAsync(HistoryQuery) const;
  Task<GitResult<CommitDetails>> ReadCommitAsync(ObjectId) const;
  Task<GitResult<TreePage>> ReadTreeAsync(TreeQuery) const;
  Task<GitResult<BlobContent>> ReadBlobAsync(BlobQuery) const;
  Task<GitResult<DiffSummary>> SummarizeDiffAsync(DiffQuery) const;
  Task<GitResult<FileDiff>> ReadPatchAsync(PatchQuery) const;
  Task<GitResult<BlamePage>> BlameAsync(BlameQuery) const;

  Task<ChangeOutcome> StageFilesAsync(StageFilesRequest) const;
  Task<ChangeOutcome> UnstageFilesAsync(UnstageFilesRequest) const;
  Task<ChangeOutcome> ApplySelectionAsync(PatchSelectionRequest) const;
  Task<CommitOutcome> CommitAsync(CommitRequest) const;
  Task<CommitOutcome> AmendAsync(AmendRequest) const;
  Task<ChangeOutcome> SwitchAsync(SwitchRequest) const;
  Task<ChangeOutcome> RestoreAsync(RestoreRequest) const;
  Task<ChangeOutcome> ResetAsync(ResetRequest) const;

  Task<GitResult<CapabilityReport>> CheckCapabilityAsync(CapabilityQuery) const;
  Task<GitResult<RemoteRefSnapshot>> ReadRemoteRefsAsync(RemoteRefQuery) const;
  GitResult<Transfer<FetchOutcome>> StartFetch(FetchRequest) const;
  GitResult<Transfer<PushOutcome>> StartPush(PushRequest) const;
  GitResult<Transfer<PullOutcome>> StartPull(PullRequest) const;

  Task<IntegrationOutcome> MergeAsync(MergeRequest) const;
  Task<IntegrationOutcome> RebaseAsync(RebaseRequest) const;
  Task<IntegrationOutcome> CherryPickAsync(CherryPickRequest) const;
  Task<IntegrationOutcome> RevertAsync(RevertRequest) const;
  Task<GitResult<std::optional<OperationSnapshot>>> ReadOperationAsync() const;
  Task<GitResult<ConflictContent>> ReadConflictAsync(ConflictQuery) const;
  Task<ResolveOutcome> ResolveConflictAsync(ResolveRequest) const;
  Task<IntegrationOutcome> ContinueAsync(ContinueRequest) const;
  Task<IntegrationOutcome> SkipAsync(SkipRequest) const;
  Task<ChangeOutcome> AbortAsync(AbortRequest) const;
};
```

`InspectAsync()` provides cheap metadata for opening the workspace. `StatusAsync()` reports staged and unstaged changes independently, can filter untracked and ignored paths, and includes the index version read. `SummarizeDiffAsync()` compares HEAD, index, worktree, or a pinned tree as permitted by the query. `ReadPatchAsync()` requests one delta under an explicit content budget and detects a stale mutable summary. A merge commit comparison uses one parent tree explicitly. A staged review summary exposes a `CommitPrecondition` from the same captured HEAD/index pair. The application carries that pair into the commit confirmation. `ReadBlobAsync()` accepts an OID and byte range or read limit, so a binary viewer or conflict editor can avoid unbounded reads. Browsing a historical patch keeps working when the current worktree changes.

A cancelled page request leaves the cursor either at its previous boundary or with the undelivered page retained for retry. Cursor position advances when the awaiting caller takes a page. Verify this with cancellation before completion and after worker completion but before UI resumption. The cursor keeps no UI state.

### Request preconditions and policies

| Request | Required inputs and state checks |
| --- | --- |
| `StageFilesRequest` and `UnstageFilesRequest` | Paths and a precondition for the entries being changed. Stage uses the selected worktree version. Unstage restores the selected HEAD-side index entries. Batch outcomes identify any applied prefix if the operation can fail after writes. |
| `PatchSelectionRequest` | `PatchAction::Stage` or `PatchAction::Unstage`, one `StagingSnapshot`, and either selected hunks or selected changed lines. Reject IDs from a different snapshot and unsupported modes. The command changes only the index. |
| `CommitRequest` and `AmendRequest` | Message, identity/signing options, and the `CommitPrecondition` of the staged review, including an unborn HEAD where applicable. Amend requires the expected commit being rewritten. Form the commit from the verified index contents and guard the ref publication against HEAD changes. If signing requires interaction, revalidate the request after the signer returns. |
| `SwitchRequest`, `RestoreRequest` and `ResetRequest` | Explicit destination, target paths where applicable, mode, and expected state for any operation that can overwrite changes. Restore distinguishes index and worktree. Reset distinguishes soft, mixed, hard, and path scope. The issue reports affected paths when a safe operation would overwrite them. The destructive request uses the version the user approved. |
| `FetchRequest` and `PullRequest` | Explicit remote, fetch refspec/tag/prune/depth options where applicable. Pull has a selected upstream and `PullMode::FastForwardOnly` by default, with `Merge` and `Rebase` opt-ins. Carry the expected current branch or HEAD, recheck it before integration, and integrate the pinned fetched OID. |
| `PushRequest` | Explicit remote and destination for each update. Identify the source by reviewed OID, or request a remote deletion explicitly. Each update chooses normal, force, or force with lease. For lease, an explicit expected old OID or expected absence is mandatory. The outcome distinguishes updated, up to date, rejected, not attempted, and unknown. |
| `MergeRequest`, `RebaseRequest`, `CherryPickRequest` and `RevertRequest` | Pin source and starting HEAD, define clean-state requirements and applicable mainline or empty-commit policy. Interactive rebase takes an ordered typed todo and an explicit compatible backend. Gate hooks, filters and signing on declared policy and available support. |

A stale write requires a fresh review before retry. In-process ordering and on-disk validation serve different purposes. For staging, compare the exact index preimage under the write lock and validate the affected source bytes as described in Stale hunk protection. For commit, a verified index/tree is the input to the write. A ref race can leave an unreferenced commit object, so the returned outcome distinguishes object creation from updating the intended ref. State used in a confirmation is tied to the command that executes it.

### Capabilities and transfer handles

`CapabilityQuery` describes the intended operation, repository requirements, remote or transport where relevant, and requested backend policy. `CapabilityReport` returns availability, the selected backend if available, and a stable reason when the operation is unavailable. Native libgit2 is the default. Desktop callers explicitly select Git CLI for an operation that requires it, and the command validates the selection again when it runs. Reports are advisory snapshots. Show the actual backend in job snapshots and outcomes. For a desktop Git credential helper, an approved adapter can run `git credential fill/approve/reject` while libgit2 performs the transfer. External commands use explicit arguments and the configured trust, hook and filter policies. [Git credential protocol](https://git-scm.com/docs/git-credential)

```cpp
template <class Outcome>
class Transfer {
public:
  TransferId Id() const noexcept;
  TransferSnapshot Snapshot() const;
  Task<Outcome> WaitAsync() const;
  TransferSubscription Observe(TaskScope,
                               std::function<void(TransferSnapshot)>) const;
  bool RequestStop() const noexcept;
  GitResult<void> AnswerAuth(AuthChallengeId, AuthAnswer) const;
};
```

`StartClone`, `StartFetch`, `StartPush` and `StartPull` synchronously validate basic inputs and enqueue without performing I/O on the application thread. The service owns jobs after admission. Handles are copyable and each `WaitAsync()` returns its own single-consumer HuxerUI task over the retained terminal outcome. Cancelling one wait or destroying one subscription detaches that consumer. `RequestStop()` is idempotent and returns whether a new stop request was accepted. Job cleanup and the terminal report follow the actual transfer.

`Observe` queues an initial snapshot and coalesced updates onto the supplied scope. The subscription is move-only and scoped to its observer. Closing the scope suppresses delivery. Snapshot revisions let observers discard historic updates and recover from coalescing. A snapshot includes phase, available counters, backend when selected, and any current challenge. The application-level job list retains its handles so another window can observe the same job.

`AuthAnswer` is an owned, move-only response that supplies scoped credential material, approves the fingerprint in a first-use SSH challenge, or declines. Answers are accepted once for the current challenge ID, and both accepted and rejected answers release their owned secret material when no longer needed. A challenge can expire or be invalidated by stop, a changed target, or disappearance of the app's authentication owner. A job waiting for an answer holds neither a network worker nor the repository write queue. When it resumes, it validates the target before starting a new attempt. SSH first-use trust uses the same observe and answer flow with an explicit fingerprint challenge. An invalid TLS peer or a changed known SSH host key fails closed.

`ReadRemoteRefsAsync()` queries advertised server refs without updating local tracking refs. It shares endpoint and trust validation with transfer jobs. The query can use a configured credential provider. It returns a typed `AuthRequired` challenge if interactive credentials are needed, and the caller can retry with the corresponding scoped answer. Closing a window can stop waiting for verification without changing the previously recorded push outcome.

### Conflict and operation sessions

`ReadOperationAsync()` returns an empty optional for an idle worktree. An active `OperationSnapshot` includes operation kind, session identity, origin and compatible backend, current step and relevant OIDs, conflict entries, available actions with reasons for unavailable actions, and an opaque version. Read actual Git state after reopening. Issue fresh local versions for the reconstructed session. Existing state with ambiguous or incompatible origin can remain visible for inspection and require an explicitly supported CLI continuation.

Each conflict entry has an ID scoped to the current snapshot and optional stage 1/base, stage 2/ours, and stage 3/theirs entries. Each side records its own path, mode and OID. `ReadConflictAsync()` retrieves the sides and existing resolution content with read limits. During rebase, expose the replay target and the replayed commit separately. `ResolveRequest` can choose a side, remove the path, submit owned result bytes, or accept a reviewed edit in the worktree. It carries the observed operation version and entry identity. `ResolveOutcome` carries a write report and the observed operation state when available. The index and worktree effects are reported separately when a resolution touches both.

`ContinueRequest`, `SkipRequest` and `AbortRequest` carry the observed operation version. Continue validates that required conflicts are resolved. Skip is offered only for a compatible replay operation when the backend implements it. Abort verifies the expected operation and affected worktree state. An incompatible version, skipped step, or protected edit produces a typed rejection and a fresh inspection path. An operation paused for conflicts is an expected `NeedsResolution` outcome. A cancelled view leaves the persisted Git operation available for later inspection. [Git merge](https://git-scm.com/docs/git-merge) · [Git rebase](https://git-scm.com/docs/git-rebase)

### Repository management API

Expose the following as grouped methods on `Repository`. The table states the request and result conventions for the methods beyond the main review and integration flows. Writes follow the outcome contract above. Remote operations that can block on a transfer use the same `Transfer<Outcome>` handle.

| Area | Public methods and inputs |
| --- | --- |
| References and branches | `ListRefsAsync` and `ResolveRevisionAsync` return typed refs and public OIDs. `CreateBranchAsync` takes a name and target. `RenameBranchAsync`, `DeleteBranchAsync` and `SetUpstreamAsync` take the selected refs and expected targets. Compare upstream tips through `CompareReachabilityAsync`. |
| Tags | `ListTagsAsync` returns lightweight and annotated tags with their peeled targets where applicable. `CreateTagAsync` takes a target OID and optional annotation/signing policy. `DeleteTagAsync` guards the expected tag. Remote tag deletion uses push. |
| Remotes | `ListRemotesAsync` returns remote IDs, addresses and refspecs. `AddRemoteAsync`, `UpdateRemoteAsync`, `RenameRemoteAsync` and `RemoveRemoteAsync` take explicit changes and protect the expected remote configuration. |
| Stash | `ListStashesAsync`, `ReadStashDiffAsync`, `SaveStashAsync`, `ApplyStashAsync`, `PopStashAsync` and `DropStashAsync`. Existing entries use the selected OID and list version, with explicit handling of untracked files and staged-state restoration. |
| Submodules | `ListSubmodulesAsync` and `OpenSubmoduleAsync` use repository-relative paths. `StartAddSubmodule` and `StartUpdateSubmodules` expose progress and per-child outcomes. `InitSubmoduleAsync` and `SyncSubmoduleAsync` handle local configuration. Stage the gitlink through the file staging API. |
| Worktrees | `ListWorktreesAsync`, `OpenWorktreeAsync`, `AddWorktreeAsync`, `LockWorktreeAsync`, `UnlockWorktreeAsync`, `RemoveWorktreeAsync` and `PruneWorktreesAsync`. Use resolved worktree identities, expected branch and dirty state, and retained access for any separate directory. |
| Diagnostics and compatibility | `ReadConfigAsync` and `SetConfigAsync` take explicit config scope for writes. `ReadReflogAsync`, `ReadAttributesAsync`, `CheckIgnoreAsync`, `ExportPatchAsync`, and `ApplyPatchAsync` expose bounded inspection or a designated patch target with expected state. `VerifySignatureAsync` returns signature availability and trust separately. Queries and patch writes report resource limits and unsupported forms explicitly. |
| Maintenance and LFS | Gate `StartMaintenance` by supported mode, such as verify or garbage collection. Gate `StartLfsFetch`, `StartLfsPull`, `StartLfsPush`, and lock lookup or mutation by the configured LFS backend. LFS-aware clone and push use the same transfer results and disclose the backend. |

### Client call sequences

**Stage a displayed hunk and commit the reviewed index.** Two windows may share the repository. Use the patch each window displayed. A stale result prompts that window to reload. The next stage of the workflow shows an index diff and retains its exact review version for the commit action. A confirmation retains its own review version across background refreshes.

```cpp
Task<void> StageSelected(Repository repo, StagingSnapshot shown,
                         std::vector<HunkId> selected,
                         std::shared_ptr<WorkspaceModel> model) {
  auto outcome = co_await repo.ApplySelectionAsync({
      .action = PatchAction::Stage,
      .snapshot = std::move(shown),
      .selection = std::move(selected),
  });
  model->PresentChange(outcome);
  co_await model->RefreshAsync(repo);
}

Task<void> CommitReviewed(Repository repo, CommitRequest reviewed,
                           std::shared_ptr<WorkspaceModel> model) {
  auto outcome = co_await repo.CommitAsync(std::move(reviewed));
  model->PresentCommit(outcome);
  co_await model->RefreshAsync(repo);
}
```

**Close a window during push.** The application starts the job and gives the window a view of the same handle. Closing the window drops its observer and wait task. The app's jobs view retains the handle and can still present authentication, request stop, or observe the final outcome. After a push with an unknown remote result, use `ReadRemoteRefsAsync()` to reconcile destination OIDs before offering retry.

```cpp
void StartPushFromWorkspace(Repository repo, PushRequest request,
                            WorkspaceModel& model, TransferList& transfers) {
  auto started = repo.StartPush(std::move(request));
  if (!started.Succeeded()) {
    model.ShowGitError(started.Error());
    return;
  }
  transfers.Add(std::move(started).Value());
}

Task<void> ShowPushOutcome(Transfer<PushOutcome> push,
                           std::shared_ptr<TransferViewModel> model) {
  model->Present(co_await push.WaitAsync());
}
```

**Continue a rebase after restart.** Reopen the repository, call `ReadOperationAsync()`, and restore the active operation view. Use the loaded snapshot to read individual conflicts. Save each resolution against the currently reviewed operation and entry. Reload `ReadOperationAsync()` after a write and pass the displayed version into `ContinueAsync()`. If another window or process advanced the session, present the rejection and refresh. The same sequence exposes `SkipAsync()` or `AbortAsync()` only when the recovered state allows it.

```cpp
Task<void> ContinueReviewedOperation(Repository repo, OperationVersion reviewed,
                                     std::shared_ptr<WorkspaceModel> model) {
  auto outcome = co_await repo.ContinueAsync({.expected = std::move(reviewed)});
  model->PresentIntegration(outcome);
  auto current = co_await repo.ReadOperationAsync();
  model->PresentOperation(std::move(current));
}
```

Move-only tasks and cursors, copyable repository and transfer handles, and owned DTOs keep these call sequences usable from the public SDK. The examples use application model methods only to render or refresh state.

## Execution and repository consistency

### HuxerUI task integration

| Work | Execution |
| --- | --- |
| UI state, prompts, confirmations | HuxerUI application thread. Use `UseTaskScope()` for view-owned queries and `UseApplicationTaskScope()` for jobs that can outlive a window. |
| Short local operations | A shared `WorkerSequence` per repository group is sufficient for the initial local prototype. Copies share one queue and operations begin in order. Active work receives a stop token and retains the sequence until it exits. |
| Long local queries, transfer, CLI | Use a bounded Git service scheduler. Reserve capacity for interactive local work, bound clone/fetch overlap, and limit packbuilder threads. Route mutations through one repository-aware admission point. |
| Small CPU postprocessing | Use `RunWorker` for bounded work. Send full-repository topology, large diffs, and blame to the Git service to protect SDK worker capacity. |
| Result and progress delivery | Use `AwaitCallback` for local results and each `Transfer::WaitAsync()` consumer. It resumes the awaiting task on the application thread. `Transfer::Observe()` uses `TaskScope::Post()` for independent progress updates. |

`Task<T>` is lazy and single-consumer. Both `RunWorker` and `WorkerSequence` use HuxerUI's process-wide worker pool, which has at most four threads in the inspected implementation. Multiple blocking transfers, or workers waiting for authentication, can occupy the entire pool. `WorkerSequence` provides ordering for one shared identity without worker-thread affinity. Multi-handle and linked-worktree coordination belongs in the Git service. See the SDK task and worker references below.

`AwaitCallback` starts on the owning application thread and returns a cancellation action. Its starter only queues work or registers completion. An already-submitted completion can lose its UI continuation without invoking the cancellation action. The job owns all cleanup independently of the task scope. C callbacks receive owned job state, catch exceptions before returning to C, and avoid application state and UI objects.

### Cancellation and recovery

1. **Queued.** Remove the job before entry into libgit2, then release its inputs and grant.
2. **Waiting.** Cancelling the HuxerUI task suppresses its continuation. For a transfer, it detaches that wait while the service retains the job. Retire or reassign pending authentication prompts if their owner closes.
3. **Stopping.** An explicit stop request sets a job signal. Transfer callbacks return a cancellation code where supported. Use `git_remote_stop` only through a tested path that preserves the transport's lifetime. Configure finite connect and I/O timeouts where the backend supports them. Show “Stopping” until the job has exited. DNS, handshake, blocking I/O, pack and checkout each need a responsiveness test. [Remote stop](https://libgit2.org/docs/reference/v1.9.0/remote/git_remote_stop.html) · [Smart transport cancellation](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/transports/smart_protocol.c)
4. **Writes already occurred.** Wait for the job to disconnect, release libgit2 handles and clean its owned temporary files. Refresh the index, refs and operation state. Fetch may already have stored objects or updated refs. For a push with a lost reply, inspect the remote target OIDs before deciding whether to retry. Clone into an exclusively owned sibling directory and publish only after success, subject to destination and filesystem checks. On restart, recognize resumable Git state and leave unfamiliar lockfiles for diagnosis. Merge and rebase conflicts remain available for explicit continue or abort. [Clone cleanup](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/clone.c#L631)

A cancellation reports what stopped and which writes are known. The operation never promises automatic rollback. Keep checkout and abort preconditions conservative because these commands can affect both the index and the worktree. Refresh after failure even if the UI cancelled its original wait.

### Repository and worktree identities

Identify a repository group by the canonical common Git directory and available filesystem identity. Identify each worktree by its own git directory, index and work directory. Use one in-process write queue per group initially. Share it among handles and windows for refs, config, stash, worktree management, fetch/push, index writes, commit, checkout and integration. A remote operation waiting for user input must release its worker and the queue. Revalidate its plan on retry.

Use independent libgit2 handles for parallel reads of fixed commit, tree, or blob IDs. Order live status, conflict inspection and writable diff snapshots with local mutations. Refresh external state by combining post-operation refresh, focus or resume refresh, and periodic rechecks. Desktop watcher events can invalidate views and should be debounced. Inspect per-worktree HEAD/index/operation state and common refs/packed refs/config. Recheck fully on watcher overflow or directory replacement. Keep the user's selection and scroll anchor while discarding results that arrive for an old view.

Git's own locks and expected ref values are the cross-process coordination points. An in-process queue cannot bind an external Git process or editor. libgit2's ref transaction documents sequential updates, so a failure can leave a prefix applied. There is no atomic commit spanning refs, index and working files. [Git worktree layout](https://git-scm.com/docs/git-worktree) · [Common directory implementation](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/repository.c#L3253) · [Reference transaction](https://github.com/libgit2/libgit2/blob/v1.9.7/include/git2/transaction.h#L97)

### Stale hunk protection

**Write only the result derived from the reviewed content, after validating its preconditions.** Keep UI generations for discarding stale results and disk-level preconditions for mutations.

- Bind an editable diff to the repository and worktree, paths and modes, missing entries, index OID, HEAD OID where relevant, a worktree content hash, and the applicable transformations. Hunk and line IDs are scoped to that snapshot. Read actual bytes for validation. Timestamps and sizes provide a quick check only.
- Stage by applying selected worktree edits to the verified index version. Unstage by restoring the selected HEAD-side edits into that version. Prepare deterministic result bytes using the Git object representation. Handle partial file creation or deletion explicitly. Cover CRLF, BOM, missing trailing newline, modes and filter behavior.
- Prototype a single `git_apply(..., GIT_APPLY_LOCATION_INDEX)` call. In v1.9.7, the reader is prepared before the lock, then the index writer locks before delta callbacks. Inside the locked callback, compare a fresh disk index with the exact loaded index version and recheck source preconditions. The full index version prevents overwriting an unrelated external `git add` entry. Prove this ordering and error cleanup under injected races. If it cannot satisfy the contract, evaluate a narrow, controlled index-publication path. Gate partial staging on the verified implementation. [Apply lock and callback order](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/apply.c#L802) · [Index writer lock](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/index.c#L3847)
- On `StaleSnapshot`, reload and let the user select from the new diff. The worktree can change after the last verification point, so publish only the index bytes derived from the reviewed snapshot and do not write the worktree in this operation. A post-operation refresh shows later edits separately.

### Progress authentication and trust

Capture fixed-size phase/counter snapshots in transfer, pack, and push callbacks. Coalesce updates per job with at most one pending UI notification. Start with five to ten updates per second. Deliver phase changes and the final outcome promptly. Indicate unknown totals and treat remote sideband as untrusted text. Limit its size and sanitize it before display or logging. [Remote callbacks](https://libgit2.org/docs/reference/v1.9.0/remote/git_remote_callbacks.html)

Try cached credentials synchronously inside the libgit2 callback. On a cache miss, publish the `AuthChallenge` on the existing job and end that attempt. `Transfer::AnswerAuth()` lets the service revalidate the target and start another attempt under the same handle. If a remote write might already have occurred, preserve the uncertain outcome and check its refs before retrying. [Remote credential callback](https://libgit2.org/docs/reference/v1.9.0/remote/git_remote_callbacks.html) · [Git credentials](https://git-scm.com/docs/gitcredentials)

Enforce HTTPS certificate-chain and hostname checks with the selected TLS backend. Support enterprise trust via configured CA roots. Validate SSH host keys with known hosts. Display the fingerprint for explicit first-time trust and reject a changed known key by default. Expose agent, key file and passphrase options according to backend capability. Use Keychain, Windows Credential Manager, Linux Secret Service, or keystore-encrypted Android storage for saved credentials. Redact URL userinfo, token-bearing queries, headers, passphrases, key data and helper output before they reach logs. [libgit2 SSH validation](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/transports/ssh_libssh2.c) · [Apple Keychain](https://developer.apple.com/documentation/security/keychain-services) · [Android Keystore](https://developer.android.com/privacy-and-security/keystore)

## Platform support and packaging

`File` is a local UTF-8 path value. `FileReference` retains an access capability and may expose a path through `AsFile()`. Keep the reference for the session and every in-flight job where the path relies on its grant. `RememberAsync()` and `RestoreAsync()` manage the persisted selection, with revoked and read-only grants reflected in the client. If a provider gives no suitable path, offer a clone into app-owned storage or import an explicit private copy. A repository import validates `.git`, gitfiles, links and grants, including paths outside a linked worktree's selected directory. See the SDK file contract in Repository references.

| Platform | Build and repository access | Synchronization and shipping |
| --- | --- | --- |
| Windows | CMake build targeting Windows and repositories on the local filesystem. Test long and Unicode paths, case behavior, symlinks and rename failure when a file is in use. | Enable the selected WinHTTP/Schannel and SSH backend. Ship with the existing installer shell, verify ABI/CRT and dependent libraries, then install and run on a clean machine. |
| macOS | Support ordinary directories and sandbox-scoped selections. Retain security-scoped URLs, handle bookmarks and stale permissions, and verify authorization for external gitdir/commondir paths. | Use system trust where available and validate the SSH backend. Verify bundle architectures, dependent libraries, signing and distribution. Enable the CLI route under the actual packaging policy. [Apple macOS sandbox access](https://developer.apple.com/documentation/security/accessing-files-from-the-macos-app-sandbox) |
| Linux | CMake build targeting Linux and repositories on the local filesystem. Validate access boundaries on portal selections and locks/rename on supported filesystems. | Build with selected TLS and SSH dependencies. Test system trust and agent integration. Verify AppImage, deb or rpm on supported distributions, including the minimum runtime ABI. |
| iOS | Cross-compile and start with app container repositories. The SDK can retain a scoped path from a picker, but provider coordination, placeholders, revocation, locking and rename require device testing before enabling in-place writes. | Validate foreground HTTPS before opening SSH support. Test app suspension, termination and grant recovery. Link required device and simulator slices through the Xcode shell and install on a real device. [Apple directory access](https://developer.apple.com/documentation/uikit/providing-access-to-directories) |
| Android | Cross-compile for supported NDK ABIs and start with app-private storage. SAF `content://` grants expose documents. Import or export copies rather than writing a repository through a provider URI. | Verify INTERNET permission, CA trust, proxies and cancellation for the selected HTTPS backend. Test SSH per ABI and packaged APK/AAB. Verify permission revocation and restore behavior. [Android Storage Access Framework](https://developer.android.com/training/data-storage/shared/documents-files) |
| Web | Prototype WASM and local read-only repository inspection. Browser handles do not provide arbitrary host paths. HuxerUI uses IDBFS, and direct libgit2 writes need an explicit persistence test and synchronization path. The current SDK worker entry reports unavailable on Web. | Use a dedicated browser worker and, separately, a browser transport or controlled backend for Git Smart HTTP. Browser CORS, authentication, stream handling, concurrent tabs, persistence, quotas and crash recovery are release gates. Arbitrary remotes and direct SSH are outside the initial browser scope. [Emscripten filesystem](https://emscripten.org/docs/api_reference/Filesystem-API.html) · [Emscripten networking](https://emscripten.org/docs/porting/networking.html) · [Fetch CORS](https://fetch.spec.whatwg.org/#http-cors-protocol) |

### CMake and mcpp integration

**CMake.** Keep `huxerui_add_library`, `HuxerUI::Git`, and the preview's existing consumer pattern. Use a pinned libgit2 source release or a dependency package built for the target toolchain. Enable C and configure TLS, SSH, zlib and the relevant platform libraries explicitly. Resolve libgit2 after the HuxerUI plan-only and graph-only early returns. Keep its headers private and propagate static link dependencies to the final consumer. Include libgit2's GPLv2 linking exception and notices for bundled dependencies in the redistributed package. [libgit2 CMake targets](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/CMakeLists.txt) · [Build options](https://github.com/libgit2/libgit2/tree/v1.9.7#building-libgit2---using-cmake) · [libgit2 license text](https://github.com/libgit2/libgit2/blob/v1.9.7/COPYING)

**mcpp.** Keep one source of public headers. Declare `mcpp.toml`, generate the module interface through the HuxerUI library template, and use `huxerui.rules` in `build.mcpp`. Provide libgit2 and its link dependencies per target triple and pass `link-search` and `link-lib` to the final consumer. Verify the independent library, preview and packaged application as a single chain. Check the target sysroot on Linux and the final Xcode link on iOS. Keep backend capabilities consistent where both systems can provide them and record the actual capabilities of each shipped build.

## Reference client workflows

Use SourceTree as the reference for workflow depth. Users should be able to follow a change from the commit graph through diff review, selective staging, commit, and synchronization, with conflict resolution and recovery available in the same workspace.

The [Git Preview desktop interaction design](git-preview-interaction-design.md) specifies the next iteration of workspace restoration, branch menus, keyboard navigation, and synchronization presentation.

| User workflow | Lib-Git calls and application state |
| --- | --- |
| Open and organize repositories | `OpenAsync → InspectAsync`. Load sidebar sections through ref, tag, remote, stash, submodule and worktree queries. Persist recent repositories, grants, groups, tabs, selections and scroll positions in the app. |
| Browse topology and review a commit | `OpenHistoryAsync` and `NextAsync` page commit summaries. `ReadCommitAsync` loads details. `SummarizeDiffAsync` lists changed files against the selected parent, and `ReadPatchAsync` loads one file. Keep lane layout, OID navigation, diff layout and selection in the app. |
| Review and commit working changes | `StatusAsync` shows staged and unstaged files independently. Stage files, hunks or lines against their displayed snapshots. Keep the commit draft, identity controls and amend preference in app state. Carry the reviewed `CommitPrecondition` from the staged summary into `CommitAsync` or `AmendAsync`. |
| Synchronize | Check capability for the intended backend and remote, then start fetch, pull or push. Keep the transfer handle in the application-level jobs view. Present authentication, per-ref rejection and unknown remote outcomes as actionable states. Use `ReadRemoteRefsAsync` to reconcile an uncertain push. |
| Resolve and resume | Load `ReadOperationAsync` and use `ReadConflictAsync` to display the sides and editable result. Save the selected resolution, refresh the operation, then continue, skip or abort with the observed version. Use the same flow after reopening. Label the replay target and replayed commit by OID during rebase. |

### Commit graph and large diffs

HuxerUI's `SplitLayout`, `TreeView`, `VirtualList`, `VirtualLayout`, `ScrollController` and `Canvas` support the desktop layout. Use stable commit IDs, fixed-height virtual rows, viewport-level drawing, and lanes that carry across pages. Ref updates can start a new query while preserving the selected commit and scroll anchor. libgit2's topological sort can preprocess the graph before the first row, and reverse walking consumes the walk up front. Benchmark the first result separately from scroll performance. If needed, make the active branch usable while the full graph builds in the background and expose that state. [Revwalk implementation](https://github.com/libgit2/libgit2/blob/v1.9.7/src/libgit2/revwalk.c#L617)

Use one aligned row model for both diff columns, with a shared vertical layout and independent horizontal positions. Load summaries before patches and defer syntax coloring to visible rows and the available budget. Provide explicit viewing or export for very large lines, binary files, and diffs that exceed the current budget. Measure libgit2 patch memory separately from virtualized UI memory.

### Conflict editor

Start with read-only base and side views, an editable result, and per-block choices. HuxerUI has a text input client and a custom editor example. Lib-RichEditor provides editable text layout and code blocks. The remaining tooling question is a large editable merge view that keeps its sides aligned and supports cross-row selections, IME, accessibility, and responsive editing. Prototype that combination before extending the SDK. Reuse the same workflow on mobile with touch navigation and a unified diff.

## Delivery and acceptance gates

Run acceptance tests in temporary repositories. Use bare local remotes for baseline object and ref behavior. Use local Smart HTTP/SSH or a controllable protocol service for interruption, TLS, authentication, server hooks, leases and shallow cases. Use the installed Git executable to compare output, commit contents and repository state.

| Phase | Deliverable and gate |
| --- | --- |
| 0. Risk prototypes | Verify index lock ordering and stale-review rejection. Race a Smart push lease against a second writer. Confirm cleanup and application-thread delivery using two waiters, then cancel one. Check a lazy task made from a temporary repository and cursor cancellation before UI delivery. Measure topology time to first row. |
| 1. Local review and commit | Ship multi-repository views, graph, staged/unstaged diff, file and hunk staging, commit and amend. Use a 100,000-commit fixture with merges and branches. Measure time to interaction, scroll frame times and peak memory. Verify selective staging using `git diff --cached`, `git ls-files --stage` and the final `git show`. Test same-size/time edits, an external `git add`, CRLF, missing trailing newline, partial new/deleted files and linked worktrees. In two windows, change the index after one has opened a commit confirmation and verify that the confirmation rejects the stale version. |
| 2. libgit2 synchronization | Ship clone, fetch, push, pull, HTTPS/SSH, progress and authentication. Cut connections during negotiation, upload and after remote ref update but before the reply. Inspect actual remote refs after a lost reply. Verify per-ref mixed outcomes, a fetch that succeeds before pull integration is rejected, and a second window that still observes progress after the first closes. Stress simultaneous transports while measuring local interaction and HuxerUI file I/O. |
| 3. Conflict and repository management | Ship merge, linear rebase, cherry-pick, revert, stash, submodule, worktree and conflict editing. Check text, delete/modify, rename and binary cases with `git ls-files -u`, resulting parents, reflogs and operation state. Test resolve/continue, restart/continue, abort from a known clean start and protection for intervening external edits. Reject a stale continue or abort request after a different window advances the session. Recognize incompatible Git CLI rebase state. |
| 4. Complete desktop release | Ship line selection, Git CLI interactive rebase, configured signing/hooks/helpers and LFS. Use local signing keys, test hooks and helpers, and a local LFS service. Run the core workflows from installed Windows, macOS and Linux packages. Verify both CMake and mcpp consumer chains and publish the measured performance envelope. |
| 5. Mobile and browser releases | Gate iOS and Android on real-device private repository access, HTTPS, touch review, hunk staging, conflict handling, and recovery from app suspension and termination. Enable external folders by provider only after validation. Ship browser review separately from writable persistence and remote synchronization. |

The first investment should go to stale index protection, then ambiguous push recovery and resumption of an interrupted libgit2 operation. The graph's time to first result is the highest-priority performance measurement. Mobile provider semantics and final-link packaging can run as independent feasibility checks once the first desktop prototype works.

## Design decisions that keep the scope manageable

- Omit a public one-to-one C API mirror and a general-purpose backend framework. Keep use-case APIs, small internal wrappers, and a narrow CLI adapter. Add expert libgit2 handle borrowing when a real caller needs it.
- Start with one write queue per repository group and bounded execution capacity. Use Git locks, expected state, and persisted Git operation data. Introduce split scheduling for linked worktrees only if measured contention requires it. A generic rollback journal would duplicate Git state without making the worktree and remote transactional.
- Keep a local path, a retained grant, and an explicit private-copy workflow. A general `FileReference` filesystem shim would need to emulate locking, atomic replacement, symlinks, and repository layout across incompatible providers.
- Keep graph layout, editor state, diff alignment, prompts, and view navigation in the example. Keep commit topology, guarded changes, conflict stages, progress and operation results in Lib-Git.

## Repository references

Lib-Git files are linked relative to this document. HuxerUI and the other independent libraries are identified by checkout name and source path. The installed SDK version was inspected separately.

- **Lib-Git integration and preview** · [CMakeLists.txt:65](../CMakeLists.txt#L65) · [HuxerUIProject.cmake:15](../HuxerUIProject.cmake#L15) · [include/huxerui/git.h:5](../include/huxerui/git.h#L5) · [src/git.cpp:5](../src/git.cpp#L5) · [examples/preview/CMakeLists.txt:49](../examples/preview/CMakeLists.txt#L49) · [examples/preview/src/app.cpp:6](../examples/preview/src/app.cpp#L6)
- **Comparable library targets** · `Lib-CSS/CMakeLists.txt:18` · `Lib-AVKit/CMakeLists.txt:31` · `Lib-RichEditor/CMakeLists.txt:66` · `HuxerUI/cmake/HuxerUILibraries.cmake:214`
- **Task and scheduling contracts** · `HuxerUI/include/huxerui/task.h:585` · `HuxerUI/include/huxerui/task.h:898` · `HuxerUI/include/huxerui/task.h:1017` · `HuxerUI/src/runtime/task.cpp:34` · `HuxerUI/src/runtime/task.cpp:503` · `HuxerUI/docs/design/tasks.md:137` · `HuxerUI/include/huxerui/app.h:219`
- **Files and provider access** · `HuxerUI/include/huxerui/file.h:94` · `HuxerUI/include/huxerui/file.h:358` · `HuxerUI/platform/macos/macos_file_picker.mm:261` · `HuxerUI/platform/ios/ios_file_picker.mm:329` · `HuxerUI/platform/linux/linux_file_picker.cpp:1081` · `HuxerUI/platform/windows/win32_file_picker.cpp:201` · `HuxerUI/platform/android/huxerui/src/main/java/org/huxerui/HuxerUIFileAccessStore.java:73` · `HuxerUI/platform/web/web_file.js:37`
- **UI primitives and examples** · `HuxerUI/include/huxerui/view.h:1074` · `HuxerUI/include/huxerui/virtual_layout.h:37` · `HuxerUI/include/huxerui/scroll.h:134` · `HuxerUI/include/huxerui/text_input.h:395` · `HuxerUI/examples/virtual_table/main.cpp:38` · `HuxerUI/examples/custom_editor/main.cpp:1019` · `Lib-RichEditor/include/huxerui/richeditor.h:414`
- **Build and packaging** · `HuxerUI/templates/library/mcpp.toml.in` · `HuxerUI/templates/library/build.mcpp` · `HuxerUI/buildtools/mcpp/README.md:45` · `HuxerUI/docs/design/mcpp-build-system.md:237` · [examples/preview/platform/android/app/build.gradle:140](../examples/preview/platform/android/app/build.gradle#L140) · [examples/preview/platform/ios/Scripts/build_huxerui_core.sh:83](../examples/preview/platform/ios/Scripts/build_huxerui_core.sh#L83)
- **Installed SDK version** · `~/Library/Developer/HuxerUI/lib/cmake/HuxerUI/HuxerUIConfigVersion.cmake:12`
