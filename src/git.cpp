#include "git_internal.h"

#include <huxerui/app.h>

#include <git2/sys/repository.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace huxerui::git::detail {

struct RepositoryGroup {
  struct Pending {
    bool transport;
    std::function<void()> work;
  };
  bool active = false;
  std::deque<Pending> pending;
};

namespace {

/// The two queues share admission state so a repository never waits on a blocked worker.
class Runtime {
public:
  Runtime() {
    Check(git_libgit2_init(), "Initialize libgit2");
    try {
      git_libgit2_opts(GIT_OPT_SET_SERVER_CONNECT_TIMEOUT, 15000);
      git_libgit2_opts(GIT_OPT_SET_SERVER_TIMEOUT, 30000);
#ifdef HUXERUI_GIT_ANDROID_MBEDTLS
      // Load the system CA store. libgit2 still validates the chain and hostname for every HTTPS connection.
      if (git_libgit2_opts(GIT_OPT_SET_SSL_CERT_LOCATIONS, nullptr, "/apex/com.android.conscrypt/cacerts") < 0) {
        git_libgit2_opts(GIT_OPT_SET_SSL_CERT_LOCATIONS, nullptr, "/system/etc/security/cacerts");
      }
#endif
#ifndef __EMSCRIPTEN__
      for (unsigned worker = 0; worker < 4; ++worker) {
        threads_.emplace_back([this, transport = worker >= 2] { Run(transport); });
      }
      threads_.emplace_back([this] { RunDeadlines(); });
#endif
    } catch (...) {
      Stop();
      git_libgit2_shutdown();
      throw;
    }
  }

  ~Runtime() {
    Stop();
    git_libgit2_shutdown();
  }

  std::shared_ptr<RepositoryGroup> Group(const std::string& path) {
    std::scoped_lock lock(mutex_);
    auto& group = groups_[path];
    auto existing = group.lock();
    if (!existing) {
      existing = std::make_shared<RepositoryGroup>();
      group = existing;
    }
    return existing;
  }

  void Submit(std::shared_ptr<RepositoryGroup> group, bool transport, std::function<void()> work) {
#ifdef __EMSCRIPTEN__
    Fail(ErrorCode::Unsupported, "Repository workers are unavailable in this Web build");
#else
    {
      std::scoped_lock lock(mutex_);
      if (group && group->active) {
        group->pending.push_back({transport, std::move(work)});
      } else {
        Queue(transport, {group, std::move(work)});
        if (group) {
          group->active = true;
        }
      }
      ++outstanding_;
    }
    condition_.notify_all();
#endif
  }

  void At(std::chrono::steady_clock::time_point when, std::function<void()> work) {
    {
      std::scoped_lock lock(mutex_);
      deadlines_.emplace(when, std::move(work));
    }
    condition_.notify_all();
  }

private:
  struct Job {
    std::shared_ptr<RepositoryGroup> group;
    std::function<void()> work;
  };

  void Queue(bool transport, Job job) {
    queues_[transport].push_back(std::move(job));
  }

  void Run(bool transport) {
    auto& queue = queues_[transport];
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] { return stopping_ || !queue.empty(); });
        if (stopping_ && queue.empty()) {
          return;
        }
        job = std::move(queue.front());
        queue.pop_front();
      }
      job.work();
      {
        std::scoped_lock lock(mutex_);
        if (job.group && !job.group->pending.empty()) {
          auto next = std::move(job.group->pending.front());
          job.group->pending.pop_front();
          Queue(next.transport, {job.group, std::move(next.work)});
        } else if (job.group) {
          job.group->active = false;
        }
        --outstanding_;
      }
      condition_.notify_all();
    }
  }

  void RunDeadlines() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
      if (deadlines_.empty()) {
        condition_.wait(lock);
        continue;
      }
      auto next = deadlines_.begin();
      if (next->first > std::chrono::steady_clock::now()) {
        condition_.wait_until(lock, next->first);
        continue;
      }
      auto work = std::move(next->second);
      deadlines_.erase(next);
      lock.unlock();
      work();
      lock.lock();
    }
  }

  void Stop() {
    {
      std::unique_lock lock(mutex_);
      condition_.wait(lock, [&] { return outstanding_ == 0; });
      stopping_ = true;
    }
    condition_.notify_all();
    for (auto& thread : threads_) {
      thread.join();
    }
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  std::array<std::deque<Job>, 2> queues_;
  std::multimap<std::chrono::steady_clock::time_point, std::function<void()>> deadlines_;
  std::unordered_map<std::string, std::weak_ptr<RepositoryGroup>> groups_;
  std::vector<std::thread> threads_;
  std::size_t outstanding_ = 0;
  bool stopping_ = false;
};

Runtime& WorkerRuntime() {
  static Runtime runtime;
  return runtime;
}

git_oid Hash(std::string_view data) {
  git_oid id{};
  Check(git_odb_hash(&id, data.data(), data.size(), GIT_OBJECT_BLOB), "Hash file contents");
  return id;
}

int LockIndex(const fs::path& path) {
#ifdef _WIN32
  return _wopen(path.c_str(), _O_BINARY | _O_WRONLY | _O_CREAT | _O_EXCL, _S_IREAD | _S_IWRITE);
#else
  return open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
#endif
}

void CloseFile(int descriptor) {
#ifdef _WIN32
  _close(descriptor);
#else
  close(descriptor);
#endif
}

bool WriteFile(int descriptor, std::span<const char> data) {
  while (!data.empty()) {
#ifdef _WIN32
    const int count = _write(descriptor, data.data(), static_cast<unsigned>(data.size()));
#else
    const auto count = write(descriptor, data.data(), data.size());
#endif
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    data = data.subspan(static_cast<std::size_t>(count));
  }
  return true;
}

bool SyncFile(int descriptor) {
#ifdef _WIN32
  return _commit(descriptor) == 0;
#else
  return fsync(descriptor) == 0;
#endif
}

fs::path Canonical(const fs::path& path) {
  std::error_code error;
  auto canonical = fs::weakly_canonical(path, error);
  if (error) {
    Fail(ErrorCode::Io, "Resolve repository directory: " + error.message());
  }
  return canonical;
}

} // namespace

void EnsureRuntime() {
  static_cast<void>(WorkerRuntime());
}

void Enqueue(std::shared_ptr<RepositoryGroup> group, bool transport, std::function<void()> work) {
  WorkerRuntime().Submit(std::move(group), transport, std::move(work));
}

void AtDeadline(std::chrono::steady_clock::time_point time, std::function<void()> work) {
  WorkerRuntime().At(time, std::move(work));
}

void Fail(ErrorCode code, std::string message) {
  throw Failure{{code, std::move(message)}};
}

std::string RedactUrl(std::string_view url) {
  url = url.substr(0, url.find_first_of("?#"));
  const auto scheme = url.find("://");
  if (scheme != std::string_view::npos) {
    const auto authority = scheme + 3;
    const auto end = url.find('/', authority);
    const auto userinfo = url.substr(authority, end - authority).rfind('@');
    if (userinfo != std::string_view::npos) {
      return std::string(url.substr(0, authority)) + std::string(url.substr(authority + userinfo + 1));
    }
  }
  return std::string(url);
}

GitError LastError(int code, std::string_view context, bool transport) {
  const auto* error = git_error_last();
  ErrorCode kind = transport ? ErrorCode::Network : ErrorCode::Git;
  switch (code) {
    case GIT_ENOTFOUND:
    case GIT_EUNBORNBRANCH: kind = ErrorCode::NotFound; break;
    case GIT_EEXISTS: kind = ErrorCode::AlreadyExists; break;
    case GIT_ELOCKED: kind = ErrorCode::Busy; break;
    case GIT_EMODIFIED: kind = ErrorCode::StaleSnapshot; break;
    case GIT_ECONFLICT:
    case GIT_EUNMERGED: kind = ErrorCode::Conflict; break;
    case GIT_EAUTH: kind = ErrorCode::AuthenticationRequired; break;
    case GIT_ECERTIFICATE: kind = ErrorCode::TrustFailed; break;
    case GIT_ENONFASTFORWARD: kind = ErrorCode::NonFastForward; break;
    case GIT_EINVALID:
    case GIT_EINVALIDSPEC: kind = ErrorCode::InvalidArgument; break;
    case GIT_EBAREREPO:
    case GIT_ENOTSUPPORTED: kind = ErrorCode::Unsupported; break;
    case GIT_EOWNER: kind = ErrorCode::PermissionDenied; break;
    case GIT_EUSER: kind = ErrorCode::Cancelled; break;
    default:
      if (error && error->klass == GIT_ERROR_OS && !transport) {
        kind = ErrorCode::Io;
      }
      break;
  }
  std::string message(context);
  // Transport diagnostics can include headers or secrets supplied by a remote.
  if (!transport && error && error->message) {
    std::string_view diagnostic(error->message);
    const auto scheme = diagnostic.find("://");
    if (scheme == std::string_view::npos && diagnostic.find("Authorization:") == std::string_view::npos) {
      message.append(": ").append(diagnostic.substr(0, 512));
    }
  }
  return {kind, std::move(message), code};
}

void Check(int code, std::string_view context) {
  if (code < 0) {
    throw Failure{LastError(code, context)};
  }
}

WriteReport Rejected(GitError error) {
  return {WriteStatus::Rejected, std::move(error)};
}

WriteReport Stopped() {
  return {WriteStatus::Stopped, GitError{ErrorCode::Cancelled, "Stop requested"}};
}

WriteReport Failed(GitError error) {
  switch (error.code) {
    case ErrorCode::Cancelled: return Stopped();
    case ErrorCode::StaleSnapshot:
    case ErrorCode::Busy:
    case ErrorCode::InvalidArgument:
    case ErrorCode::Unsupported:
    case ErrorCode::PermissionDenied:
    case ErrorCode::LocalChangesWouldBeOverwritten:
    case ErrorCode::NoUpstream:
    case ErrorCode::NonFastForward: return Rejected(std::move(error));
    default: return {WriteStatus::Failed, std::move(error)};
  }
}

std::string PathString(const fs::path& path) {
  const auto utf8 = path.generic_u8string();
  return std::string(utf8.begin(), utf8.end());
}

fs::path Utf8Path(std::string_view path) {
  return fs::path(std::u8string(path.begin(), path.end()));
}

File PathFile(const fs::path& path) {
  return File(PathString(path));
}

bool IsWithin(const fs::path& parent, const fs::path& child) {
  const auto relative = child.lexically_relative(parent);
  return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

std::pair<File, std::optional<FileReference>> ResolveSource(RepositorySource source, bool write) {
  if (const auto* file = std::get_if<File>(&source)) {
    return {*file, std::nullopt};
  }
  auto reference = std::get<FileReference>(std::move(source));
  if (reference.Type() != FileType::Directory) {
    Fail(ErrorCode::InvalidArgument, "Choose a repository directory");
  }
  if (write && !reference.CanWrite()) {
    Fail(ErrorCode::PermissionDenied, "The selected directory is read-only");
  }
  auto file = reference.AsFile();
  if (!file) {
    Fail(ErrorCode::Unsupported, "This directory provider does not expose a local filesystem path");
  }
  return {*file, std::move(reference)};
}

Repo Open(const RepositoryState& state, bool write) {
  if (write && !state.writable) {
    Fail(ErrorCode::PermissionDenied, "The repository was opened with read-only access");
  }
  const auto path = state.location.git_directory.Path();
  return Make<git_repository, git_repository_free>("Open repository", git_repository_open_ext, path.c_str(),
                                                   GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr);
}

std::shared_ptr<RepositoryState> BindRepository(git_repository* repo, std::optional<FileReference> access) {
  const auto gitdir = Canonical(Utf8Path(git_repository_path(repo)));
  std::optional<File> workdir;
  if (const auto* path = git_repository_workdir(repo)) {
    workdir = PathFile(Canonical(Utf8Path(path)));
  }
  const auto common = Canonical(Utf8Path(git_repository_commondir(repo)));
  if (access) {
    const auto granted = access->AsFile();
    if (!granted) {
      Fail(ErrorCode::Unsupported, "This directory grant does not expose a local filesystem path");
    }
    const auto root = Canonical(Utf8Path(granted->Path()));
    if (!IsWithin(root, gitdir) || !IsWithin(root, common) ||
        (workdir && !IsWithin(root, Utf8Path(workdir->Path())))) {
      Fail(ErrorCode::PermissionDenied, "Git metadata or the working directory is outside the retained directory grant");
    }
  }
  const bool writable = !access || access->CanWrite();
  return std::make_shared<RepositoryState>(
      RepositoryState{{PathFile(gitdir), std::move(workdir), std::move(access)}, writable,
                      WorkerRuntime().Group(PathString(common))});
}

ObjectId Id(const git_oid* id) {
  std::array<char, GIT_OID_HEXSZ + 1> hex{};
  git_oid_tostr(hex.data(), hex.size(), id);
  return {hex.data()};
}

std::optional<ObjectId> OptionalId(const git_oid* id) {
  return id && !git_oid_is_zero(id) ? std::optional(Id(id)) : std::nullopt;
}

git_oid Parse(const ObjectId& id) {
  git_oid parsed{};
  if (id.algorithm != HashAlgorithm::Sha1 || id.hex.size() != GIT_OID_HEXSZ ||
      git_oid_fromstr(&parsed, id.hex.c_str()) < 0) {
    Fail(ErrorCode::InvalidArgument, "Expected a full SHA-1 object ID");
  }
  return parsed;
}

Object Revision(git_repository* repo, std::string_view name) {
  RequireString(name, "Revision");
  const std::string text(name);
  return Make<git_object, git_object_free>("Resolve revision", git_revparse_single, repo, text.c_str());
}

Commit FindCommit(git_repository* repo, const ObjectId& id) {
  const auto oid = Parse(id);
  return Make<git_commit, git_commit_free>("Read commit", git_commit_lookup, repo, &oid);
}

Tree CommitTree(git_commit* commit) {
  return Make<git_tree, git_tree_free>("Read commit tree", git_commit_tree, commit);
}

Tree HeadTree(git_repository* repo) {
  const auto head = ReadHead(repo);
  if (!head.target) {
    return {nullptr, git_tree_free};
  }
  auto commit = FindCommit(repo, *head.target);
  return CommitTree(commit.get());
}

HeadState ReadHead(git_repository* repo) {
  auto ref = Make<git_reference, git_reference_free>("Read HEAD", git_reference_lookup, repo, "HEAD");
  if (git_reference_type(ref.get()) == GIT_REFERENCE_DIRECT) {
    return {HeadKind::Detached, "HEAD", OptionalId(git_reference_target(ref.get()))};
  }
  HeadState head{HeadKind::Unborn, git_reference_symbolic_target(ref.get()), {}};
  git_reference* resolved = nullptr;
  const int code = git_reference_resolve(&resolved, ref.get());
  Ref reference(resolved, git_reference_free);
  if (code == GIT_ENOTFOUND) {
    return head;
  }
  Check(code, "Resolve HEAD");
  head.kind = HeadKind::Attached;
  head.reference = git_reference_name(reference.get());
  head.target = OptionalId(git_reference_target(reference.get()));
  return head;
}

Signature ReadSignature(const git_signature* signature) {
  if (!signature) {
    return {};
  }
  return {signature->name, signature->email, std::chrono::sys_seconds{std::chrono::seconds{signature->when.time}},
          signature->when.offset};
}

Handle<git_signature, git_signature_free> MakeSignature(git_repository* repo,
                                                        const std::optional<Signature>& signature) {
  if (!signature) {
    return Make<git_signature, git_signature_free>("Read commit identity", git_signature_default, repo);
  }
  RequireString(signature->name, "Author name");
  RequireString(signature->email, "Author email");
  if (!signature->time) {
    return Make<git_signature, git_signature_free>("Create commit identity", git_signature_now,
                                                   signature->name.c_str(), signature->email.c_str());
  }
  return Make<git_signature, git_signature_free>("Create commit identity", git_signature_new,
      signature->name.c_str(), signature->email.c_str(), signature->time->time_since_epoch().count(),
      signature->time_offset_minutes);
}

void RequireString(std::string_view value, std::string_view description) {
  if (value.empty() || value.find('\0') != std::string_view::npos) {
    Fail(ErrorCode::InvalidArgument, std::string(description) + " must be nonempty and contain no NUL");
  }
}

void RequireRefName(std::string_view value) {
  RequireString(value, "Reference name");
  const std::string name(value);
  int valid = 0;
  Check(git_reference_name_is_valid(&valid, name.c_str()), "Validate reference name");
  if (!valid || !value.starts_with("refs/")) {
    Fail(ErrorCode::InvalidArgument, "Expected a full Git reference name");
  }
}

std::string ValidatePath(std::string_view path) {
  RequireString(path, "Repository path");
  if (path.front() == '/' || path.back() == '/' || path.find('\\') != std::string_view::npos) {
    Fail(ErrorCode::InvalidArgument, "Expected a repository-relative path using forward slashes");
  }
  for (std::size_t offset = 0; offset < path.size();) {
    const auto end = path.find('/', offset);
    const auto component = path.substr(offset, end - offset);
    std::string folded(component);
    std::ranges::transform(folded, folded.begin(), [](unsigned char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
    });
    if (folded.empty() || folded == "." || folded == ".." || folded == ".git" ||
        folded.find(':') != std::string::npos || folded.back() == ' ' ||
        (folded.back() == '.' && folded.find("git") != std::string::npos)) {
      Fail(ErrorCode::InvalidArgument, "Repository path contains a reserved component");
    }
    if (end == std::string_view::npos) {
      break;
    }
    offset = end + 1;
  }
  return std::string(path);
}

Bytes ReadBytes(const fs::path& path, std::size_t limit) {
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error || size > limit || size > std::size_t(std::numeric_limits<std::streamsize>::max())) {
    Fail(error ? ErrorCode::Io : ErrorCode::Unsupported,
         error ? "Cannot read repository file: " + error.message() : "File exceeds the requested byte limit");
  }
  std::ifstream input(path, std::ios::binary);
  Bytes bytes(static_cast<std::size_t>(size));
  if (!input || (size && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) ||
      input.peek() != std::char_traits<char>::eof()) {
    Fail(ErrorCode::StaleSnapshot, "File changed while its contents were being read");
  }
  return bytes;
}

std::string Fingerprint(const fs::path& path) {
  std::error_code error;
  const bool exists = fs::exists(path, error);
  if (error) {
    Fail(ErrorCode::Io, "Inspect repository metadata: " + error.message());
  }
  if (!exists) {
    return {};
  }
  const auto bytes = ReadBytes(path, 512 * 1024 * 1024);
  const auto hash = Hash(std::string_view(bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data()),
                                          bytes.size()));
  return Id(&hash).hex;
}

Index ReadIndex(git_repository* repo) {
  auto index = Make<git_index, git_index_free>("Read index", git_repository_index, repo);
  Check(git_index_read(index.get(), 1), "Reload index");
  return index;
}

fs::path IndexPath(git_repository* repo) {
  auto index = ReadIndex(repo);
  const auto* path = git_index_path(index.get());
  if (!path) {
    Fail(ErrorCode::Unsupported, "This repository does not have an on-disk index");
  }
  return Utf8Path(path);
}

std::shared_ptr<const VersionState> ReadVersion(git_repository* repo) {
  return std::make_shared<const VersionState>(VersionState{git_repository_path(repo), ReadHead(repo),
                                                          Fingerprint(IndexPath(repo))});
}

bool MatchesVersion(git_repository* repo, const VersionState& version) {
  return version.git_directory == git_repository_path(repo) && version.head == ReadHead(repo) &&
         version.index == Fingerprint(IndexPath(repo));
}

IndexEdit::IndexEdit(git_repository* repo) : repo_(repo), index_(IndexPath(repo)), lock_(index_) {
  lock_ += ".lock";
  descriptor_ = LockIndex(lock_);
  if (descriptor_ < 0) {
    const int error = errno;
    Fail(error == EEXIST ? ErrorCode::Busy : ErrorCode::Io,
         error == EEXIST ? "Git index is locked" : "Could not acquire the Git index lock");
  }
  owns_lock_ = true;
}

IndexEdit::~IndexEdit() {
  edit_.reset();
  if (descriptor_ >= 0) {
    CloseFile(descriptor_);
  }
  std::error_code error;
  if (owns_lock_) {
    fs::remove(lock_, error);
  }
  if (!temporary_.empty()) {
    fs::remove_all(temporary_, error);
  }
}

git_index* IndexEdit::Edit() {
  if (edit_) {
    return edit_.get();
  }
  static std::atomic<std::uint64_t> revision{0};
  std::error_code error;
  for (int attempt = 0; attempt < 32; ++attempt) {
    auto candidate = index_.parent_path() / ("huxerui-index-" + std::to_string(++revision));
    if (fs::create_directory(candidate, error)) {
      temporary_ = std::move(candidate);
      break;
    }
    if (error) {
      Fail(ErrorCode::Io, "Create an isolated index: " + error.message());
    }
  }
  if (temporary_.empty()) {
    Fail(ErrorCode::Busy, "Could not reserve an isolated index");
  }
  const auto working = temporary_ / "index";
  if (fs::exists(index_, error)) {
    fs::copy_file(index_, working, error);
  }
  if (error) {
    Fail(ErrorCode::Io, "Copy the locked index: " + error.message());
  }
  edit_ = Make<git_index, git_index_free>("Open isolated index", git_index_open, PathString(working).c_str());
  Check(git_repository_set_index(repo_, edit_.get()), "Associate isolated index with repository");
  return edit_.get();
}

void IndexEdit::Publish() {
  Check(git_index_write(Edit()), "Serialize isolated index");
  std::ifstream input(temporary_ / "index", std::ios::binary);
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), buffer.size());
    if (!WriteFile(descriptor_, std::span(buffer.data(), static_cast<std::size_t>(input.gcount())))) {
      Fail(ErrorCode::Io, "Write the Git index lock file");
    }
  }
  if (!input.eof() || !SyncFile(descriptor_)) {
    Fail(ErrorCode::Io, "Flush the Git index lock file");
  }
  CloseFile(descriptor_);
  descriptor_ = -1;
  std::error_code error;
  fs::rename(lock_, index_, error);
  if (error) {
    Fail(ErrorCode::Io, "Publish the Git index: " + error.message());
  }
  owns_lock_ = false;
}

void RequireNoExternalFilters(git_repository* repo, std::string_view path) {
  for (const auto* name : {"filter", "working-tree-encoding"}) {
    const char* value = nullptr;
    Check(git_attr_get(&value, repo, 0, std::string(path).c_str(), name), "Read file attributes");
    if (GIT_ATTR_HAS_VALUE(value) || GIT_ATTR_IS_TRUE(value)) {
      Fail(ErrorCode::Unsupported, "This path requires a Git filter or encoding backend: " + std::string(path));
    }
  }
}

void RequireNoClientHooks(git_repository* repo, std::initializer_list<const char*> names) {
  auto config = Make<git_config, git_config_free>("Read Git configuration", git_repository_config_snapshot, repo);
  const char* configured = nullptr;
  const int code = git_config_get_string(&configured, config.get(), "core.hooksPath");
  if (code != GIT_ENOTFOUND) {
    Check(code, "Read configured hooks directory");
  }
  auto directory = Utf8Path(git_repository_commondir(repo)) / "hooks";
  if (configured && *configured) {
    directory = Utf8Path(configured);
    if (!directory.is_absolute()) {
      const auto* worktree = git_repository_workdir(repo);
      directory = Utf8Path(worktree ? worktree : git_repository_path(repo)) / directory;
    }
  }
  for (const auto* name : names) {
    std::error_code error;
    const auto entry = fs::status(directory / name, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      Fail(ErrorCode::Io, "Cannot inspect configured Git hooks");
    }
    if (!fs::is_regular_file(entry)) {
      continue;
    }
#ifndef _WIN32
    const auto executables = fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
    if ((entry.permissions() & executables) == fs::perms::none) {
      continue;
    }
#endif
    Fail(ErrorCode::Unsupported, std::string("This operation requires a Git backend that runs the ") + name + " hook");
  }
}

void RequireUnsignedCommitPolicy(git_repository* repo) {
  auto config = Make<git_config, git_config_free>("Read Git configuration", git_repository_config_snapshot, repo);
  int sign = 0;
  const int code = git_config_get_bool(&sign, config.get(), "commit.gpgsign");
  if (code != GIT_ENOTFOUND) {
    Check(code, "Read commit signature policy");
  }
  if (sign) {
    Fail(ErrorCode::Unsupported, "This repository requires signed commits");
  }
}

} // namespace huxerui::git::detail

namespace huxerui::git {
using namespace detail;

void Install(ApplicationContext& context) {
  context.Provide(std::make_shared<GitService>());
}

Capabilities GitService::GetCapabilities() const noexcept {
#if defined(__EMSCRIPTEN__)
  return {};
#else
  const auto features = git_libgit2_features();
  return {.local = true, .https = (features & GIT_FEATURE_HTTPS) != 0,
          .ssh = (features & GIT_FEATURE_SSH) != 0, .shallow = true, .interactive_rebase = GitCliSupported(),
          .signing = true, .git_lfs = GitCliSupported()};
#endif
}

Task<GitResult<RepositoryLocation>> GitService::DiscoverAsync(RepositorySource source,
                                                              std::optional<File> boundary) const {
  return Dispatch<GitResult<RepositoryLocation>>(nullptr,
      [source = std::move(source), boundary = std::move(boundary)](std::stop_token) mutable {
        try {
          auto [file, access] = ResolveSource(std::move(source), false);
          const auto path = file.Path();
          auto repo = Make<git_repository, git_repository_free>("Discover repository", git_repository_open_ext,
                                                                 path.c_str(), 0, nullptr);
          auto state = BindRepository(repo.get(), std::move(access));
          if (boundary) {
            const auto outer = Canonical(Utf8Path(boundary->Path()));
            const auto& found = state->location.work_directory.value_or(state->location.git_directory);
            if (!IsWithin(outer, Utf8Path(found.Path()))) {
              Fail(ErrorCode::NotFound, "No repository was found within the discovery boundary");
            }
          }
          return GitResult<RepositoryLocation>::Success(std::move(state->location));
        } catch (const Failure& error) {
          return GitResult<RepositoryLocation>::Failure(error.error);
        }
      });
}

Task<GitResult<Repository>> GitService::OpenAsync(RepositorySource source) const {
  return Dispatch<GitResult<Repository>>(nullptr, [source = std::move(source)](std::stop_token) mutable {
    try {
      auto [file, access] = ResolveSource(std::move(source), false);
      const auto path = file.Path();
      auto repo = Make<git_repository, git_repository_free>("Open repository", git_repository_open_ext, path.c_str(),
                                                           GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr);
      return GitResult<Repository>::Success(Repository(BindRepository(repo.get(), std::move(access))));
    } catch (const Failure& error) {
      return GitResult<Repository>::Failure(error.error);
    }
  });
}

Task<GitResult<Repository>> GitService::OpenAsync(RepositoryLocation location) const {
  return Dispatch<GitResult<Repository>>(nullptr, [location = std::move(location)](std::stop_token) mutable {
    try {
      const auto path = location.git_directory.Path();
      auto repo = Make<git_repository, git_repository_free>("Open discovered repository", git_repository_open_ext,
          path.c_str(), GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr);
      return GitResult<Repository>::Success(Repository(BindRepository(repo.get(), std::move(location.access))));
    } catch (const Failure& error) {
      return GitResult<Repository>::Failure(error.error);
    }
  });
}

Task<GitResult<Repository>> GitService::InitAsync(RepositorySource source, bool bare) const {
  return Dispatch<GitResult<Repository>>(nullptr, [source = std::move(source), bare](std::stop_token) mutable {
    try {
      auto [file, access] = ResolveSource(std::move(source), true);
      const auto path = file.Path();
      git_repository_init_options options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
      options.flags = GIT_REPOSITORY_INIT_MKPATH | GIT_REPOSITORY_INIT_NO_REINIT |
                      (bare ? GIT_REPOSITORY_INIT_BARE : 0);
      auto repo = Make<git_repository, git_repository_free>("Initialize repository", git_repository_init_ext,
                                                           path.c_str(), &options);
      return GitResult<Repository>::Success(Repository(BindRepository(repo.get(), std::move(access))));
    } catch (const Failure& error) {
      return GitResult<Repository>::Failure(error.error);
    }
  });
}

} // namespace huxerui::git
