#pragma once

#include <huxerui/git.h>

#include <git2.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <type_traits>
#include <utility>

namespace huxerui::git::detail {

namespace fs = std::filesystem;

struct Failure {
  GitError error;
};

[[noreturn]] void Fail(ErrorCode, std::string);

/// Copies the failure diagnostic on the thread that made the libgit2 call.
/// @param code Return code from the failed call.
/// @param context Description of the attempted operation.
/// @param transport Use a portable diagnostic without transport-provided error text.
/// @return An owned error value suitable for delivery to another thread.
GitError LastError(int code, std::string_view context, bool transport = false);

void Check(int, std::string_view context);

WriteReport Rejected(GitError);

WriteReport Failed(GitError);

WriteReport Stopped();

std::string RedactUrl(std::string_view);

template <class T, void (*Free)(T*)>
using Handle = std::unique_ptr<T, decltype(Free)>;

template <class T, void (*Free)(T*), class F, class... Args>
Handle<T, Free> Make(std::string_view context, F function, Args&&... args) {
  T* value = nullptr;
  const int code = function(&value, std::forward<Args>(args)...);
  Handle<T, Free> result(value, Free);
  Check(code, context);
  return result;
}

using Repo = Handle<git_repository, git_repository_free>;
using Index = Handle<git_index, git_index_free>;
using Ref = Handle<git_reference, git_reference_free>;
using Commit = Handle<git_commit, git_commit_free>;
using Tree = Handle<git_tree, git_tree_free>;
using Object = Handle<git_object, git_object_free>;
using Remote = Handle<git_remote, git_remote_free>;

/// A repository group shares refs and objects across all of its worktrees.
struct RepositoryGroup;
struct RepositoryState {
  RepositoryLocation location;
  bool writable;
  std::shared_ptr<RepositoryGroup> group;
};

/// Queues work without blocking the calling thread.
/// @param group Shared repository identity for ordered work, or null for an independent job.
/// @param transport Use the workers reserved for blocking transfers when true.
/// @param work Owned callable that returns after releasing its libgit2 handles.
void Enqueue(std::shared_ptr<RepositoryGroup> group, bool transport, std::function<void()> work);

/// Runs a short expiry action on the service's deadline thread.
/// @param when Earliest time at which to invoke work.
/// @param work Callback that must not wait for an application thread or hold a repository slot.
void AtDeadline(std::chrono::steady_clock::time_point when, std::function<void()> work);

void EnsureRuntime();

std::string PathString(const fs::path&);

fs::path Utf8Path(std::string_view);

File PathFile(const fs::path&);

bool IsWithin(const fs::path& parent, const fs::path& child);

/// Resolves a source to a local path and retains any grant that made it accessible.
/// @param source Path or directory reference.
/// @param write Require write access when the source carries a directory grant.
/// @return The path and its access grant, if one was supplied.
std::pair<File, std::optional<FileReference>> ResolveSource(RepositorySource source, bool write);

Repo Open(const RepositoryState&, bool write = false);

/// Binds canonical directories, the retained grant, and the shared ordering group.
/// @param repo Open repository borrowed for this call.
/// @param access Optional grant. The checkout, Git directory, and common Git directory must be within it.
/// @return State that allows each job to open its own libgit2 handle.
std::shared_ptr<RepositoryState> BindRepository(git_repository* repo, std::optional<FileReference> access);

ObjectId Id(const git_oid*);

std::optional<ObjectId> OptionalId(const git_oid*);

FileDelta ReadDelta(const git_diff_delta&);

Handle<git_diff, git_diff_free> MakeDiff(git_repository*, const DiffQuery&);

git_oid Parse(const ObjectId&);

Object Revision(git_repository*, std::string_view);

Commit FindCommit(git_repository*, const ObjectId&);

Tree CommitTree(git_commit*);

Tree HeadTree(git_repository*);

HeadState ReadHead(git_repository*);

Signature ReadSignature(const git_signature*);

Handle<git_signature, git_signature_free> MakeSignature(git_repository*, const std::optional<Signature>&);

std::string ValidatePath(std::string_view);

void RequireString(std::string_view, std::string_view description);

void RequireRefName(std::string_view);

std::string Fingerprint(const fs::path& path);

fs::path IndexPath(git_repository*);

Index ReadIndex(git_repository*);

void RequireNoExternalFilters(git_repository*, std::string_view path);

void RequireNoClientHooks(git_repository*, std::initializer_list<const char*> hooks);

void RequireUnsignedCommitPolicy(git_repository*);

void RequireCheckoutFilters(git_repository*, git_tree*, const std::vector<std::string>& paths = {});

WorktreeVersion ReadWorktreeVersion(git_repository*);

/// Restores the index from a tree under Git's index lock, using literal path matching.
/// @param repo Repository whose index will be edited.
/// @param tree Source tree, or null for an unborn HEAD.
/// @param paths Validated file and directory paths to restore.
/// @param stop Stop signal checked before publishing the replacement index.
/// @param outcome Index effect and write status reported to the caller.
void RestoreIndexPaths(git_repository* repo, git_tree* tree, const git_strarray& paths,
                       std::stop_token stop, ChangeOutcome& outcome);

Bytes ReadBytes(const fs::path&, std::size_t limit);

struct VersionState {
  std::string git_directory;
  HeadState head;
  std::string index;
};

std::shared_ptr<const VersionState> ReadVersion(git_repository*);

bool MatchesVersion(git_repository*, const VersionState&);

/// Owns Git's index lock and an isolated index used for preparing a replacement.
class IndexEdit {
public:
  /// Acquires the repository's Git index lock. The repository must outlive this edit.
  /// @param repo Repository whose on-disk index will be changed.
  explicit IndexEdit(git_repository* repo);

  IndexEdit(const IndexEdit&) = delete;

  IndexEdit& operator=(const IndexEdit&) = delete;

  ~IndexEdit();

  const fs::path& Path() const noexcept { return index_; }

  /// @return The owned editable index loaded while the on-disk lock is held.
  git_index* Edit();

  /// Flushes the replacement through the lock file, then renames it over the live index.
  void Publish();

private:
  git_repository* repo_;
  fs::path index_;
  fs::path lock_;
  fs::path temporary_;
  int descriptor_ = -1;
  bool owns_lock_ = false;
  Index edit_{nullptr, git_index_free};
};

std::optional<OperationKind> Operation(git_repository*);

std::optional<OperationSnapshot> ReadOperation(git_repository*);

IntegrationOutcome Integrate(git_repository*, const ObjectId&, PullMode, std::stop_token = {},
                             const std::optional<HeadState>& expected = {});

void SafeFastForward(git_repository*, const HeadState&, git_commit*);

void SetHeadTarget(git_repository*, const HeadState&, const git_oid*, const char*, const git_signature* = nullptr);

void CommitIntegration(git_repository*, std::optional<std::string>, IntegrationOutcome&,
                       const std::optional<HeadState>& expected = {});

bool GitCliSupported() noexcept;

void RunLfs(git_repository*, const LfsRequest&, const GitCliOptions&, std::stop_token, ChangeOutcome&);

/// Bridges owned worker work to an application-thread Task.
/// @param group Repository group to order against, or null for independent work.
/// @param work Callable that accepts a stop token and returns the operation's value.
/// @param transport Use the transfer executor for potentially blocking transport work.
/// @return A lazy Task. Cancellation skips queued work or signals the worker after it starts.
template <class Value, class Work>
Task<Value> Dispatch(std::shared_ptr<RepositoryGroup> group, Work work, bool transport = false) {
  return AwaitCallback<Value>([group = std::move(group), work = std::move(work), transport]
                              (CallbackCompletion<Value> completion) mutable {
    auto stop = std::make_shared<std::stop_source>();
    try {
      Enqueue(std::move(group), transport, [completion, stop, work = std::move(work)]() mutable {
        if (stop->stop_requested()) {
          return;
        }
        try {
          completion.Complete(work(stop->get_token()));
        } catch (...) {
          completion.Fail(std::current_exception());
        }
      });
    } catch (const Failure& error) {
      if constexpr (requires { Value::Failure(error.error); }) {
        completion.Complete(Value::Failure(error.error));
      } else {
        Value outcome;
        outcome.report = Failed(error.error);
        completion.Complete(std::move(outcome));
      }
    }
    return [stop] { stop->request_stop(); };
  });
}

/// Performs a read with a dedicated libgit2 handle.
/// @param state Retained repository state and access grant.
/// @param work Query accepting the open repository and a stop token.
/// @param immutable Allow independent execution for reads of immutable Git objects.
/// @return A Task yielding the query data or a Git error.
template <class Work>
auto Query(std::shared_ptr<RepositoryState> state, Work work, bool immutable = false) {
  using Value = std::invoke_result_t<Work, git_repository*, std::stop_token>;
  auto group = immutable ? nullptr : state->group;
  return Dispatch<GitResult<Value>>(std::move(group), [state = std::move(state), work = std::move(work)]
                                                     (std::stop_token stop) mutable {
    try {
      auto repository = Open(*state);
      return GitResult<Value>::Success(work(repository.get(), stop));
    } catch (const Failure& error) {
      return GitResult<Value>::Failure(error.error);
    }
  });
}

/// Performs a write in repository order while keeping any effects recorded before failure.
/// @param state Retained repository state and access grant.
/// @param work Command accepting the open repository, its outcome object, and a stop token.
/// @return A Task yielding the outcome, including any diagnostic reported by the operation.
template <class Outcome, class Work>
Task<Outcome> Write(std::shared_ptr<RepositoryState> state, Work work) {
  auto group = state->group;
  return Dispatch<Outcome>(std::move(group), [state = std::move(state), work = std::move(work)]
                                             (std::stop_token stop) mutable {
    Outcome outcome;
    try {
      auto repository = Open(*state, true);
      work(repository.get(), outcome, stop);
    } catch (const Failure& error) {
      const auto backend = outcome.report.backend;
      outcome.report = Failed(error.error);
      outcome.report.backend = backend;
    }
    return outcome;
  });
}

} // namespace huxerui::git::detail
