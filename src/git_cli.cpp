#include "git_internal.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if (defined(__APPLE__) && TARGET_OS_OSX) || (defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__))
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif

namespace huxerui::git::detail {
namespace {

std::string ShellPath(const File& file) {
  std::string quoted = "'";
  for (const char c : file.Path()) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

std::vector<char*> Pointers(std::vector<std::string>& values) {
  std::vector<char*> pointers;
  for (auto& value : values) {
    pointers.push_back(value.data());
  }
  pointers.push_back(nullptr);
  return pointers;
}

int Run(git_repository* repo, const GitCliOptions& options, std::vector<std::string> command,
        std::stop_token stop, const std::optional<File>& todo = {}) {
  if (stop.stop_requested()) {
    Fail(ErrorCode::Cancelled, "Git compatibility command stopped");
  }
  const auto program = options.executable.Path();
  if (access(program.c_str(), X_OK) != 0) {
    Fail(ErrorCode::Unsupported, "The selected Git executable is unavailable");
  }
  const auto* workdir = git_repository_workdir(repo);
  std::vector<std::string> argv{program, "-C", workdir ? workdir : git_repository_path(repo)};
  if (!options.run_hooks) {
    argv.insert(argv.end(), {"-c", "core.hooksPath=/dev/null"});
  }
  argv.insert(argv.end(), command.begin(), command.end());

#if defined(__APPLE__)
  char** environment = *_NSGetEnviron();
#else
  char** environment = environ;
#endif
  std::vector<std::string> env;
  for (auto value = environment; value && *value; ++value) {
    if (!std::string_view(*value).starts_with("GIT_")) {
      env.emplace_back(*value);
    }
  }
  env.emplace_back("GIT_TERMINAL_PROMPT=0");
  env.emplace_back("GIT_EDITOR=" + (options.editor ? ShellPath(*options.editor) : std::string("true")));
  if (todo) {
    // Git invokes its sequence editor through a shell. Both paths come from the process, not the todo text.
    env.emplace_back("GIT_SEQUENCE_EDITOR=/bin/cp " + ShellPath(*todo));
  }
  auto args = Pointers(argv);
  auto variables = Pointers(env);

  struct SpawnOptions {
    posix_spawn_file_actions_t files;
    posix_spawnattr_t attributes;
    SpawnOptions() {
      if (posix_spawn_file_actions_init(&files) != 0) {
        Fail(ErrorCode::Io, "Initialize Git executable I/O");
      }
      if (posix_spawnattr_init(&attributes) != 0) {
        posix_spawn_file_actions_destroy(&files);
        Fail(ErrorCode::Io, "Initialize Git executable attributes");
      }
    }

    ~SpawnOptions() {
      posix_spawnattr_destroy(&attributes);
      posix_spawn_file_actions_destroy(&files);
    }
  } spawn;
  if (posix_spawn_file_actions_addopen(&spawn.files, STDIN_FILENO, "/dev/null", O_RDONLY, 0) != 0 ||
      posix_spawn_file_actions_addopen(&spawn.files, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) != 0 ||
      posix_spawn_file_actions_addopen(&spawn.files, STDERR_FILENO, "/dev/null", O_WRONLY, 0) != 0 ||
      posix_spawnattr_setflags(&spawn.attributes, POSIX_SPAWN_SETPGROUP) != 0 ||
      posix_spawnattr_setpgroup(&spawn.attributes, 0) != 0) {
    Fail(ErrorCode::Io, "Configure Git executable");
  }
  pid_t pid = 0;
  if (posix_spawn(&pid, program.c_str(), &spawn.files, &spawn.attributes, args.data(), variables.data()) != 0) {
    Fail(ErrorCode::Io, "Launch Git executable");
  }
  const auto stop_signal = [&] { kill(-pid, SIGTERM); };
  std::stop_callback cancel(stop, stop_signal);
  int status = 0;
  std::optional<std::chrono::steady_clock::time_point> stopping_since;
  for (;;) {
    const auto result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      break;
    }
    if (result == -1 && errno != EINTR) {
      Fail(ErrorCode::Io, "Wait for Git executable");
    }
    if (stop.stop_requested()) {
      const auto now = std::chrono::steady_clock::now();
      if (!stopping_since) {
        stopping_since = now;
      } else if (now - *stopping_since > std::chrono::seconds(2)) {
        kill(-pid, SIGKILL);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (stop.stop_requested()) {
    Fail(ErrorCode::Cancelled, "Git compatibility command stopped");
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

struct TodoFile {
  fs::path directory;

  explicit TodoFile(std::string_view content) {
    auto name = PathString(fs::temp_directory_path() / "huxerui-git-todo-XXXXXX");
    if (!mkdtemp(name.data())) {
      Fail(ErrorCode::Io, "Create private rebase todo directory");
    }
    directory = Utf8Path(name);
    std::ofstream out(directory / "todo", std::ios::binary);
    out << content;
    out.close();
    if (!out) {
      std::error_code error;
      fs::remove_all(directory, error);
      Fail(ErrorCode::Io, "Write rebase todo file");
    }
  }

  ~TodoFile() {
    std::error_code error;
    fs::remove_all(directory, error);
  }

  File Path() const { return PathFile(directory / "todo"); }
};

std::string Action(RebaseAction action) {
  switch (action) {
    case RebaseAction::Pick: return "pick";
    case RebaseAction::Reword: return "reword";
    case RebaseAction::Edit: return "edit";
    case RebaseAction::Squash: return "squash";
    case RebaseAction::Fixup: return "fixup";
    case RebaseAction::Drop: return "drop";
  }
  Fail(ErrorCode::InvalidArgument, "Invalid interactive rebase action");
}

std::set<std::string> ReplayedCommits(git_repository* repo, const HeadState& head, const ObjectId& upstream) {
  auto walk = Make<git_revwalk, git_revwalk_free>("Inspect rebase input", git_revwalk_new, repo);
  const auto from = Parse(*head.target);
  const auto base = Parse(upstream);
  Check(git_revwalk_push(walk.get(), &from), "Select current branch history");
  Check(git_revwalk_hide(walk.get(), &base), "Select rebase upstream");
  std::set<std::string> commits;
  for (;;) {
    git_oid id{};
    const auto code = git_revwalk_next(&id, walk.get());
    if (code == GIT_ITEROVER) {
      break;
    }
    Check(code, "Read rebase input history");
    auto commit = FindCommit(repo, Id(&id));
    if (git_commit_parentcount(commit.get()) > 1) {
      Fail(ErrorCode::Unsupported, "This interactive rebase API accepts linear history");
    }
    commits.insert(Id(&id).hex);
  }
  return commits;
}

void ReportRebase(git_repository* repo, int status, IntegrationOutcome& outcome) {
  outcome.operation = ReadOperation(repo);
  if (outcome.operation) {
    if (outcome.operation->conflicts.empty() && status != 0) {
      outcome.report = {WriteStatus::Failed, GitError{ErrorCode::Git, "Git rebase paused with an error"},
                        Backend::GitCli};
    } else {
      outcome.report.status = WriteStatus::NeedsResolution;
    }
    return;
  }
  if (status != 0) {
    Fail(ErrorCode::Git, "Git rebase did not complete");
  }
  outcome.commit = ReadHead(repo).target;
}

} // namespace

bool GitCliSupported() noexcept {
  return true;
}

void RunLfs(git_repository* repo, const LfsRequest& request, const GitCliOptions& options,
            std::stop_token stop, ChangeOutcome& outcome) {
  outcome.report.backend = Backend::GitCli;
  RequireString(request.remote, "LFS remote");
  if (request.remote.starts_with('-') || request.remote.find_first_of("\r\n") != std::string::npos) {
    Fail(ErrorCode::InvalidArgument, "Select a configured remote by name");
  }
  Make<git_remote, git_remote_free>("Find LFS remote", git_remote_lookup, repo, request.remote.c_str());
  if (Run(repo, options, {"lfs", "version"}, stop) != 0) {
    Fail(ErrorCode::Unsupported, "The selected Git installation does not provide git-lfs");
  }
  if (request.action == LfsAction::Pull && !request.references.empty()) {
    Fail(ErrorCode::InvalidArgument, "LFS pull updates the current worktree and takes no extra refs");
  }
  std::vector<std::string> arguments{"lfs", request.action == LfsAction::Fetch ? "fetch" :
      request.action == LfsAction::Pull ? "pull" : "push", request.remote};
  for (const auto& ref : request.references) {
    RequireRefName(ref);
    arguments.push_back(ref);
  }
  if (request.action == LfsAction::Pull) {
    outcome.worktree = WriteEffect::Unknown;
  }
  if (Run(repo, options, std::move(arguments), stop) != 0) {
    Fail(ErrorCode::Git, "Git LFS command failed");
  }
  if (request.action == LfsAction::Pull) {
    outcome.worktree = WriteEffect::Changed;
  }
}

namespace {

/// Validates the reviewed plan against HEAD before passing the todo to Git.
/// @param repo Repository opened for the write.
/// @param request Reviewed rebase plan.
/// @param options Git executable, editor, and hook policy.
/// @param stop Cancellation token for the Git process.
/// @param outcome Backend and resulting rebase state.
void RunInteractiveRebase(git_repository* repo, const InteractiveRebaseRequest& request,
                          const GitCliOptions& options, std::stop_token stop, IntegrationOutcome& outcome) {
  outcome.report.backend = Backend::GitCli;
  if (Operation(repo)) {
    Fail(ErrorCode::Busy, "Finish the current Git operation first");
  }
  if (!request.expected.target || request.expected != ReadHead(repo)) {
    Fail(ErrorCode::StaleSnapshot, "HEAD changed after reviewing the rebase plan");
  }
  if (request.edits.empty()) {
    Fail(ErrorCode::InvalidArgument, "Provide the reviewed rebase todo");
  }
  auto commits = ReplayedCommits(repo, request.expected, request.upstream);
  std::string todo;
  for (const auto& item : request.edits) {
    const auto oid = Parse(item.commit);
    auto hex = Id(&oid).hex;
    if (commits.erase(hex) != 1) {
      Fail(ErrorCode::StaleSnapshot, "The rebase plan includes a duplicate or a commit outside the replay range");
    }
    if ((item.action == RebaseAction::Reword || item.action == RebaseAction::Squash) && !options.editor) {
      Fail(ErrorCode::InvalidArgument, "Choose a message editor for reword or squash");
    }
    todo += Action(item.action) + ' ' + hex + '\n';
  }
  if (!commits.empty()) {
    Fail(ErrorCode::StaleSnapshot, "The rebase plan must account for every replayed commit");
  }
  const auto base = Parse(request.upstream);
  TodoFile input(todo);
  const int result = Run(repo, options,
      {"rebase", "--interactive", "--no-autostash", "--no-autosquash", "--no-rebase-merges", "--reapply-cherry-picks",
       Id(&base).hex}, stop, input.Path());
  ReportRebase(repo, result, outcome);
}

/// Continues, skips, or aborts a Git CLI rebase after verifying the current operation.
/// @param repo Repository opened for the write.
/// @param expected Version of the operation the caller reviewed.
/// @param action Rebase action requested by the caller.
/// @param options Git executable, editor, and hook policy.
/// @param stop Cancellation token for the Git process.
/// @param outcome Backend and resulting rebase state.
void RunRebaseControl(git_repository* repo, const OperationVersion& expected, RebaseControl action,
                      const GitCliOptions& options, std::stop_token stop, IntegrationOutcome& outcome) {
  outcome.report.backend = Backend::GitCli;
  const auto active = ReadOperation(repo);
  if (!active || active->version != expected) {
    Fail(ErrorCode::StaleSnapshot, "The Git operation changed before the requested action");
  }
  if (active->backend != Backend::GitCli || active->kind != OperationKind::Rebase) {
    Fail(ErrorCode::Unsupported, "The selected operation is not a Git CLI rebase");
  }
  const auto flag = action == RebaseControl::Continue ? "--continue" :
                    action == RebaseControl::Skip ? "--skip" : "--abort";
  ReportRebase(repo, Run(repo, options, {"rebase", flag}, stop), outcome);
}

} // namespace
} // namespace huxerui::git::detail

#else

namespace huxerui::git::detail {
namespace {

constexpr auto kCliUnavailable = "This platform does not provide a Git executable backend";

void RunInteractiveRebase(git_repository*, const InteractiveRebaseRequest&, const GitCliOptions&,
                          std::stop_token, IntegrationOutcome&) {
  Fail(ErrorCode::Unsupported, kCliUnavailable);
}

void RunRebaseControl(git_repository*, const OperationVersion&, RebaseControl, const GitCliOptions&,
                      std::stop_token, IntegrationOutcome&) {
  Fail(ErrorCode::Unsupported, kCliUnavailable);
}

} // namespace

bool GitCliSupported() noexcept {
  return false;
}

void RunLfs(git_repository*, const LfsRequest&, const GitCliOptions&, std::stop_token, ChangeOutcome&) {
  Fail(ErrorCode::Unsupported, kCliUnavailable);
}

} // namespace huxerui::git::detail

#endif

namespace huxerui::git {
using namespace detail;

Task<IntegrationOutcome> Repository::InteractiveRebaseAsync(InteractiveRebaseRequest request,
                                                            GitCliOptions options) const {
  return Write<IntegrationOutcome>(state_, [request = std::move(request), options = std::move(options)]
      (auto* repo, auto& outcome, auto stop) {
    RunInteractiveRebase(repo, request, options, stop, outcome);
  });
}

Task<IntegrationOutcome> Repository::ControlGitRebaseAsync(OperationVersion expected, RebaseControl action,
                                                           GitCliOptions options) const {
  return Write<IntegrationOutcome>(state_, [expected = std::move(expected), action, options = std::move(options)]
      (auto* repo, auto& outcome, auto stop) {
    RunRebaseControl(repo, expected, action, options, stop, outcome);
  });
}

} // namespace huxerui::git
