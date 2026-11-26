#include "git_internal.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace huxerui::git::detail {

struct Observer : std::enable_shared_from_this<Observer> {
  Observer(TaskScope scope, std::function<void(TransferSnapshot)> on_update)
      : scope(std::move(scope)), on_update(std::move(on_update)) {}

  void Post(const TransferSnapshot& snapshot) {
    {
      std::scoped_lock lock(mutex);
      if (!active || snapshot.revision < latest.revision) {
        return;
      }
      latest = snapshot;
      if (waiting) {
        return;
      }
      waiting = true;
    }
    auto weak = weak_from_this();
    scope.Post([weak] {
      if (auto self = weak.lock()) {
        TransferSnapshot snapshot;
        {
          std::scoped_lock lock(self->mutex);
          self->waiting = false;
          if (!self->active) {
            return;
          }
          snapshot = self->latest;
        }
        self->on_update(std::move(snapshot));
      }
    });
  }

  void Stop() {
    std::scoped_lock lock(mutex);
    active = false;
  }

  std::mutex mutex;
  TaskScope scope;
  std::function<void(TransferSnapshot)> on_update;
  TransferSnapshot latest;
  bool active = true;
  bool waiting = false;
};

namespace {

std::atomic<std::uint64_t> next_transfer{0};
std::atomic<std::uint64_t> next_challenge{0};

constexpr unsigned Mask(CredentialKind kind) {
  return static_cast<unsigned>(kind);
}

unsigned CredentialMask(unsigned types) {
  unsigned result = 0;
  if (types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) result |= Mask(CredentialKind::UserPassword);
  if (types & GIT_CREDENTIAL_SSH_KEY) result |= Mask(CredentialKind::SshKey) | Mask(CredentialKind::SshAgent);
  if (types & GIT_CREDENTIAL_USERNAME) result |= Mask(CredentialKind::Username);
  if (types & GIT_CREDENTIAL_DEFAULT) result |= Mask(CredentialKind::Default);
  return result;
}

void ValidateTransferUrl(std::string_view url) {
  RequireString(url, "Remote address");
  if (url.find_first_of("\r\n") != std::string_view::npos) {
    Fail(ErrorCode::InvalidArgument, "Remote address contains a line break");
  }
  const auto separator = url.find("://");
  if (separator == std::string_view::npos) {
    return;
  }
  const auto authority_start = separator + 3;
  const auto authority = url.substr(authority_start, url.find('/', authority_start) - authority_start);
  const auto at = authority.rfind('@');
  if (at != std::string_view::npos &&
      (!url.starts_with("ssh://") || authority.substr(0, at).find(':') != std::string_view::npos)) {
    Fail(ErrorCode::InvalidArgument, "Supply remote credentials through the authentication request");
  }
  if ((url.starts_with("https://") || url.starts_with("http://")) && url.find_first_of("?#") != std::string_view::npos) {
    Fail(ErrorCode::InvalidArgument, "An HTTP Git endpoint must not include query parameters or a fragment");
  }
}

Remote OpenRemote(git_repository* repo, const std::string& name, bool push = false) {
  RequireString(name, "Remote name");
  auto remote = Make<git_remote, git_remote_free>("Open remote", git_remote_lookup, repo, name.c_str());
  const auto* url = push ? git_remote_pushurl(remote.get()) : nullptr;
  if (!url) {
    url = git_remote_url(remote.get());
  }
  ValidateTransferUrl(url ? url : "");
  return remote;
}

bool LocalTransport(const git_remote* remote, bool push) {
  const auto* address = push ? git_remote_pushurl(remote) : nullptr;
  if (!address) {
    address = git_remote_url(remote);
  }
  const std::string_view url(address ? address : "");
  return url.starts_with("file://") ||
         (url.find("://") == std::string_view::npos && (url.starts_with('/') || url.starts_with('.') ||
          url.find(':') == std::string_view::npos ||
          (url.size() > 2 && url[1] == ':' && (url[2] == '/' || url[2] == '\\'))));
}

struct ScopedCredential {
  Credential value;
  std::string endpoint;
  bool automatic = false;
  std::string redirected_from;
};

class TransferBase {
public:
  TransferBase() {
    snapshot.id = ++next_transfer;
  }

  TransferSnapshot Snapshot() const {
    std::scoped_lock lock(mutex);
    return snapshot;
  }

  void Emit() {
    std::vector<std::shared_ptr<Observer>> listeners;
    TransferSnapshot update;
    {
      std::scoped_lock lock(mutex);
      update = snapshot;
      std::erase_if(observers, [&](const auto& weak) {
        if (auto entry = weak.lock()) {
          listeners.push_back(std::move(entry));
          return false;
        }
        return true;
      });
    }
    for (const auto& entry : listeners) {
      entry->Post(update);
    }
  }

  void Progress(TransferPhase phase, std::size_t completed = 0, std::size_t total = 0, std::size_t bytes = 0) {
    bool notify = false;
    {
      std::scoped_lock lock(mutex);
      if (stopping || snapshot.phase == TransferPhase::Finished) {
        return;
      }
      const auto now = std::chrono::steady_clock::now();
      notify = snapshot.phase != phase || now - delivered >= std::chrono::milliseconds(100);
      snapshot.phase = phase;
      snapshot.completed = completed;
      snapshot.total = total;
      snapshot.bytes = bytes;
      ++snapshot.revision;
      if (notify) {
        delivered = now;
      }
    }
    if (notify) {
      Emit();
    }
  }

  std::shared_ptr<Observer> Observe(TaskScope scope, std::function<void(TransferSnapshot)> observer) {
    auto entry = std::make_shared<Observer>(std::move(scope), std::move(observer));
    TransferSnapshot initial;
    {
      std::scoped_lock lock(mutex);
      initial = snapshot;
      observers.push_back(entry);
    }
    entry->Post(initial);
    return entry;
  }

  std::optional<ScopedCredential> TakeCredential() {
    std::scoped_lock lock(mutex);
    return std::exchange(credential, {});
  }

  bool StopRequested() const noexcept { return stopping.load(); }

  mutable std::mutex mutex;
  TransferSnapshot snapshot;
  std::vector<std::weak_ptr<Observer>> observers;
  std::chrono::steady_clock::time_point delivered{};
  std::chrono::steady_clock::time_point auth_deadline{};
  std::atomic<bool> stopping{false};
  std::stop_source stop;
  std::optional<ScopedCredential> credential;
  std::optional<AuthChallenge> requested;
  std::string redirected_from;
  unsigned attempts = 0;
  bool may_have_pushed = false;
  bool can_resolve_credential = true;
};

struct RemoteCallbacks {
  TransferBase* job;
  std::optional<ScopedCredential> credential;
  std::exception_ptr exception;
  std::vector<FetchedRef>* fetched = nullptr;
  const PushRequest* push = nullptr;
  PushOutcome* push_result = nullptr;
  bool credential_used = false;
  bool allow_credentials = true;
  std::string source_url;

  /// Prepares a retry against the HTTPS endpoint that issued the challenge.
  /// @param configured_url Current URL of the selected remote and direction.
  /// @return The in-memory URL override, or null when the configured URL can be used.
  const char* RetryUrl(std::string_view configured_url) {
    source_url = configured_url;
    allow_credentials = !configured_url.starts_with("http://");
    if (!credential || credential->redirected_from.empty()) return nullptr;
    if (configured_url != credential->redirected_from) {
      Fail(ErrorCode::StaleSnapshot, "The remote URL changed during authentication");
    }
    return credential->endpoint.c_str();
  }

  /// Applies any retry URL to the selected direction of an in-memory remote.
  /// @param remote Remote instance used for this attempt.
  /// @param push Whether the operation uses the push URL.
  void PrepareRemote(git_remote* remote, bool push) {
    const auto* url = push ? git_remote_pushurl(remote) : nullptr;
    if (const auto* retry = RetryUrl(url ? url : git_remote_url(remote))) {
      Check(push ? git_remote_set_instance_pushurl(remote, retry) : git_remote_set_instance_url(remote, retry),
            "Select the challenged HTTPS endpoint");
    }
  }

  bool BlockOffsiteRedirects() const {
    return credential && (credential->automatic || !credential->redirected_from.empty());
  }

  template <class Work>
  int Invoke(Work work) noexcept {
    try {
      if (job && job->StopRequested()) {
        return GIT_EUSER;
      }
      return work();
    } catch (...) {
      exception = std::current_exception();
      return GIT_EUSER;
    }
  }

  void Progress(TransferPhase phase, std::size_t count = 0, std::size_t total = 0, std::size_t bytes = 0) {
    if (job) {
      job->Progress(phase, count, total, bytes);
    }
  }

  int Auth(git_credential** out, const char* url, const char* username, unsigned allowed) {
    const std::string_view address(url ? url : "");
    if (!allow_credentials || address.starts_with("http://")) {
      return GIT_EAUTH;
    }
    const auto permitted = CredentialMask(allowed);
    const auto endpoint = RedactUrl(address);
    const bool https = address.starts_with("https://");
    const bool clean_https = https && address == endpoint && address.find_first_of("\r\n") == std::string_view::npos;
    if (credential && !credential_used && credential->endpoint == endpoint &&
        (!credential->automatic || !https || clean_https) &&
        (permitted & Mask(credential->value.kind))) {
      const auto& answer = credential->value;
      const char* user = answer.username.empty() ? username : answer.username.c_str();
      if (!user) {
        user = "";
      }
      credential_used = true;
      switch (answer.kind) {
        case CredentialKind::UserPassword:
          return git_credential_userpass_plaintext_new(out, user, answer.secret.c_str());
        case CredentialKind::Username:
          return git_credential_username_new(out, user);
        case CredentialKind::SshAgent:
          return git_credential_ssh_key_from_agent(out, user);
        case CredentialKind::Default:
          return git_credential_default_new(out);
        case CredentialKind::SshKey: {
          if (!answer.private_key) {
            return GIT_EAUTH;
          }
          auto public_key = answer.public_key ? answer.public_key->Path() : std::string{};
          auto private_key = answer.private_key->Path();
          return git_credential_ssh_key_new(out, user, public_key.empty() ? nullptr : public_key.c_str(),
                                            private_key.c_str(), answer.secret.c_str());
        }
      }
    }
    if (job && permitted) {
      const bool rejected = credential_used && credential->endpoint == endpoint &&
                            (permitted & Mask(credential->value.kind));
      job->requested = AuthChallenge{++next_challenge, endpoint, username ? username : "", permitted, rejected};
      job->can_resolve_credential = !https || clean_https;
      // A sanitized URL is suitable for display but cannot replace the actual transport URL.
      job->redirected_from = clean_https && source_url.starts_with("https://") && source_url != endpoint
          ? source_url : std::string{};
    }
    return GIT_EAUTH;
  }

  int Negotiation(const git_push_update** updates, std::size_t count) {
    if (!push) {
      return 0;
    }
    for (std::size_t i = 0; i < count; ++i) {
      const auto* actual = updates[i];
      const auto requested = std::ranges::find(push->updates, std::string_view(actual->dst_refname),
                                                &PushUpdate::destination);
      if (requested == push->updates.end()) {
        return GIT_EINVALIDSPEC;
      }
      if (requested->policy == PushPolicy::ForceWithLease && requested->expected != OptionalId(&actual->src)) {
        Fail(ErrorCode::NonFastForward, "The remote reference no longer matches its force-push lease");
      }
    }
    for (const auto& requested : push->updates) {
      if (requested.policy == PushPolicy::ForceWithLease &&
          std::none_of(updates, updates + count, [&](const auto* actual) {
            return actual->dst_refname == requested.destination;
          })) {
        Fail(ErrorCode::NonFastForward, "The remote did not advertise the reference selected for a force-push lease");
      }
    }
    if (job) {
      job->may_have_pushed = true;
    }
    return 0;
  }

  void CheckResult(int code, std::string_view action) {
    if (exception) {
      std::rethrow_exception(exception);
    }
    if (code != 0) {
      if (job && job->StopRequested()) {
        Fail(ErrorCode::Cancelled, "Transfer stopped");
      }
      if (job && job->requested && !job->may_have_pushed) {
        Fail(ErrorCode::AuthenticationRequired, "The remote requires authentication");
      }
      throw Failure{LastError(code, action, true)};
    }
  }

  git_remote_callbacks Bind() {
    git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
    callbacks.payload = this;
    callbacks.credentials = [](git_credential** out, const char* url, const char* user, unsigned allowed,
                               void* payload) -> int {
      auto& self = *static_cast<RemoteCallbacks*>(payload);
      return self.Invoke([&] { return self.Auth(out, url, user, allowed); });
    };
    callbacks.transfer_progress = [](const git_indexer_progress* stats, void* payload) -> int {
      auto& self = *static_cast<RemoteCallbacks*>(payload);
      return self.Invoke([&] {
        self.Progress(TransferPhase::Receiving, stats->received_objects, stats->total_objects, stats->received_bytes);
        return 0;
      });
    };
    callbacks.pack_progress = [](int, std::uint32_t current, std::uint32_t total, void* payload) -> int {
      auto& self = *static_cast<RemoteCallbacks*>(payload);
      return self.Invoke([&] { self.Progress(TransferPhase::Packing, current, total); return 0; });
    };
    callbacks.push_transfer_progress = [](unsigned current, unsigned total, std::size_t bytes, void* payload) -> int {
      auto& self = *static_cast<RemoteCallbacks*>(payload);
      return self.Invoke([&] { self.Progress(TransferPhase::Sending, current, total, bytes); return 0; });
    };
    callbacks.push_negotiation = [](const git_push_update** updates, std::size_t count, void* payload) -> int {
      auto& self = *static_cast<RemoteCallbacks*>(payload);
      return self.Invoke([&] { return self.Negotiation(updates, count); });
    };
    callbacks.push_update_reference = [](const char* reference, const char* status, void* payload) -> int {
      auto& self = *static_cast<RemoteCallbacks*>(payload);
      // Record reports even when stop is requested. They may be the only confirmed remote outcome.
      try {
        if (self.push_result) {
          const auto item = std::ranges::find(self.push_result->references, std::string_view(reference),
                                              &RefPushResult::destination);
          if (item != self.push_result->references.end()) {
            item->status = status ? RefPushStatus::Rejected : RefPushStatus::Updated;
            if (status) {
              item->diagnostic = "The remote rejected this reference update";
            }
          }
        }
        return 0;
      } catch (...) {
        self.exception = std::current_exception();
        return GIT_EUSER;
      }
    };
    callbacks.update_refs = [](const char* name, const git_oid* previous, const git_oid* target, git_refspec*,
                              void* payload) -> int {
      auto& self = *static_cast<RemoteCallbacks*>(payload);
      try {
        if (self.fetched) {
          self.fetched->push_back({name, OptionalId(previous), OptionalId(target)});
        }
        self.Progress(TransferPhase::UpdatingReferences);
        return 0;
      } catch (...) {
        self.exception = std::current_exception();
        return GIT_EUSER;
      }
    };
    return callbacks;
  }
};

} // namespace

template <class Outcome> struct TransferState final : TransferBase, std::enable_shared_from_this<TransferState<Outcome>> {
  using Command = std::function<void(TransferState&, Outcome&)>;

  void Submit() {
    auto self = this->shared_from_this();
    Enqueue(group, true, [self] { self->Execute(); });
  }

  void Execute() {
    Outcome outcome;
    bool started = false;
    {
      std::scoped_lock lock(mutex);
      if (!stopping && !result) {
        snapshot.phase = TransferPhase::Connecting;
        ++snapshot.revision;
        started = true;
      }
    }
    if (!started) {
      outcome.report = Stopped();
      Finish(std::move(outcome));
      return;
    }
    try {
      Emit();
      requested.reset();
      command(*this, outcome);
    } catch (const Failure& error) {
      const auto backend = outcome.report.backend;
      outcome.report = Failed(error.error);
      outcome.report.backend = backend;
    } catch (...) {
      outcome.report = Failed({ErrorCode::Git, "Unexpected transfer failure"});
    }

    std::optional<std::pair<std::uint64_t, std::chrono::steady_clock::time_point>> waiting;
    std::optional<CredentialRequest> lookup;
    {
      std::scoped_lock lock(mutex);
      if (!stopping && !may_have_pushed && requested && attempts < 3 &&
          outcome.report.issue && outcome.report.issue->code == ErrorCode::AuthenticationRequired) {
        auth_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        waiting.emplace(requested->id, auth_deadline);
        if (resolver && can_resolve_credential && !requested->rejected && looked_up.insert(requested->endpoint).second) {
          resolving = std::move(requested);
          lookup = credential_context;
          lookup->challenge = *resolving;
          snapshot.phase = TransferPhase::ResolvingCredentials;
        } else {
          snapshot.authentication = std::move(requested);
          snapshot.phase = TransferPhase::AwaitingAuthentication;
        }
        ++snapshot.revision;
      }
    }
    if (waiting) {
      const auto [id, time] = *waiting;
      AtDeadline(time, [weak = this->weak_from_this(), id] {
        if (auto job = weak.lock()) {
          job->Expire(id);
        }
      });
      Emit();
      if (lookup) QueueLookup(std::move(*lookup));
    } else {
      Finish(std::move(outcome));
    }
  }

  /// Queues one owned provider request without holding a repository execution slot.
  /// @param request Application context and the exact challenge to answer.
  void QueueLookup(CredentialRequest request) {
    auto weak = this->weak_from_this();
    const auto id = request.challenge.id;
    try {
      Enqueue(nullptr, false, [weak, provider = resolver, request = std::move(request), id] {
        if (auto job = weak.lock()) {
          std::scoped_lock lock(job->mutex);
          if (!job->resolving || job->resolving->id != id || job->stopping || job->result) return;
        } else {
          return;
        }
        std::optional<Credential> answer;
        try {
          answer = (*provider)(request);
        } catch (...) {
          // Provider diagnostics may include storage details. Leave authentication to the caller.
        }
        if (auto job = weak.lock()) job->CompleteLookup(id, std::move(answer));
      });
    } catch (...) {
      CompleteLookup(id, {});
    }
  }

  /// Submits the provider's answer or publishes the same challenge for manual entry.
  /// @param id Challenge that owned the lookup.
  /// @param answer Credential offered by the provider, if available.
  void CompleteLookup(std::uint64_t id, std::optional<Credential> answer) {
    if (answer && Answer(id, std::move(answer), true).Succeeded()) return;
    {
      std::scoped_lock lock(mutex);
      if (!resolving || resolving->id != id || stopping || result) return;
      snapshot.authentication = std::exchange(resolving, {});
      snapshot.phase = TransferPhase::AwaitingAuthentication;
      ++snapshot.revision;
    }
    Emit();
  }

  void Expire(std::uint64_t id) {
    {
      std::scoped_lock lock(mutex);
      const auto& challenge = resolving ? resolving : snapshot.authentication;
      if (!challenge || challenge->id != id || stopping || result) {
        return;
      }
      snapshot.authentication.reset();
      resolving.reset();
    }
    Outcome outcome;
    outcome.report = Failed({ErrorCode::AuthenticationRequired, "Authentication request expired"});
    Finish(std::move(outcome));
  }

  void Finish(Outcome outcome) {
    std::unordered_map<std::uint64_t, CallbackCompletion<Outcome>> receivers;
    {
      std::scoped_lock lock(mutex);
      if (result) {
        return;
      }
      result = std::move(outcome);
      credential.reset();
      snapshot.authentication.reset();
      resolving.reset();
      snapshot.phase = TransferPhase::Finished;
      ++snapshot.revision;
      receivers.swap(waiters);
    }
    Emit();
    for (auto& [key, receiver] : receivers) {
      receiver.Complete(*result);
    }
  }

  bool RequestStop() noexcept {
    bool waiting = false;
    {
      std::scoped_lock lock(mutex);
      if (result || stopping.exchange(true)) {
        return false;
      }
      waiting = snapshot.phase == TransferPhase::AwaitingAuthentication ||
                snapshot.phase == TransferPhase::ResolvingCredentials || snapshot.phase == TransferPhase::Queued;
      snapshot.authentication.reset();
      snapshot.phase = TransferPhase::Stopping;
      ++snapshot.revision;
    }
    stop.request_stop();
    try {
      if (waiting) {
        Outcome outcome;
        outcome.report = Stopped();
        Finish(std::move(outcome));
      } else {
        Emit();
      }
    } catch (...) {
      // Stop is already requested even if the application can no longer accept a notification.
    }
    return true;
  }

  GitResult<void> Answer(std::uint64_t id, std::optional<Credential> answer, bool automatic = false) {
    bool declined = false;
    {
      std::scoped_lock lock(mutex);
      const auto& challenge = automatic ? resolving : snapshot.authentication;
      if (!challenge || challenge->id != id || stopping || result) {
        return GitResult<void>::Failure({ErrorCode::StaleSnapshot, "This authentication request is no longer active"});
      }
      declined = !answer || std::chrono::steady_clock::now() > auth_deadline;
      if (!declined && !(challenge->allowed & Mask(answer->kind))) {
        return GitResult<void>::Failure({ErrorCode::InvalidArgument, "Credential type is unavailable for this request"});
      }
      credential = declined ? std::nullopt
                            : std::optional(ScopedCredential{std::move(*answer), challenge->endpoint, automatic,
                                                             std::move(redirected_from)});
      if (automatic) {
        resolving.reset();
      }
      snapshot.authentication.reset();
      snapshot.phase = TransferPhase::Queued;
      ++snapshot.revision;
      ++attempts;
    }
    if (declined) {
      Outcome outcome;
      outcome.report = Failed({ErrorCode::AuthenticationRequired, "Authentication was declined or expired"});
      Finish(std::move(outcome));
      return GitResult<void>::Success();
    }
    try {
      Submit();
    } catch (const Failure& failure) {
      Outcome outcome;
      outcome.report = Failed(failure.error);
      Finish(std::move(outcome));
      return GitResult<void>::Failure(failure.error);
    }
    Emit();
    return GitResult<void>::Success();
  }

  std::shared_ptr<RepositoryGroup> group;
  std::shared_ptr<const CredentialProvider> resolver;
  CredentialRequest credential_context;
  std::optional<AuthChallenge> resolving;
  std::unordered_set<std::string> looked_up;
  Command command;
  std::optional<Outcome> result;
  std::uint64_t next_waiter = 0;
  std::unordered_map<std::uint64_t, CallbackCompletion<Outcome>> waiters;
};

namespace {

template <class Outcome, class Work>
std::shared_ptr<TransferState<Outcome>> Start(std::shared_ptr<RepositoryGroup> group, Work work,
                                              std::shared_ptr<const CredentialProvider> resolver = {},
                                              CredentialRequest context = {}) {
  auto state = std::make_shared<TransferState<Outcome>>();
  state->group = std::move(group);
  state->command = std::move(work);
  state->resolver = std::move(resolver);
  state->credential_context = std::move(context);
  state->Submit();
  return state;
}

std::vector<char*> Strings(const std::vector<std::string>& values) {
  std::vector<char*> result;
  for (const auto& value : values) {
    RequireString(value, "Refspec");
    result.push_back(const_cast<char*>(value.c_str()));
  }
  return result;
}

void ConfigureFetch(git_fetch_options& options, const FetchRequest& request, RemoteCallbacks& callbacks) {
  if (request.depth > static_cast<unsigned>(std::numeric_limits<int>::max()) ||
      (request.unshallow && request.depth)) {
    Fail(ErrorCode::InvalidArgument, "Invalid shallow fetch depth");
  }
  options.depth = request.unshallow ? GIT_FETCH_DEPTH_UNSHALLOW : static_cast<int>(request.depth);
  options.prune = request.prune ? GIT_FETCH_PRUNE : GIT_FETCH_NO_PRUNE;
  options.callbacks = callbacks.Bind();
  if (callbacks.BlockOffsiteRedirects()) options.follow_redirects = GIT_REMOTE_REDIRECT_NONE;
}

std::optional<ObjectId> AdvertisedUpstream(git_remote* remote, const FetchRequest& request,
                                          const std::string& tracking_ref) {
  struct SourceName {
    git_buf value = GIT_BUF_INIT;
    ~SourceName() { git_buf_dispose(&value); }
  } source;
  const auto match = [&](const git_refspec* spec) {
    if (!spec || git_refspec_direction(spec) != GIT_DIRECTION_FETCH ||
        !git_refspec_dst_matches(spec, tracking_ref.c_str())) {
      return false;
    }
    return git_refspec_rtransform(&source.value, spec, tracking_ref.c_str()) == 0;
  };
  if (request.refspecs.empty()) {
    for (std::size_t i = 0; i < git_remote_refspec_count(remote); ++i) {
      if (match(git_remote_get_refspec(remote, i))) break;
    }
  } else {
    for (const auto& text : request.refspecs) {
      auto spec = Make<git_refspec, git_refspec_free>("Read fetch refspec", git_refspec_parse, text.c_str(), 1);
      if (match(spec.get())) break;
    }
  }
  if (!source.value.size) {
    return {};
  }
  const git_remote_head** heads = nullptr;
  std::size_t count = 0;
  if (git_remote_ls(&heads, &count, remote) == 0) {
    const std::string_view wanted(source.value.ptr, source.value.size);
    for (std::size_t i = 0; i < count; ++i) {
      if (wanted == heads[i]->name) return OptionalId(&heads[i]->oid);
    }
  }
  return {};
}

std::optional<ObjectId> Fetch(git_repository* repo, TransferBase& job, const FetchRequest& request, FetchOutcome& outcome,
                              const std::string& tracking_ref = {}) {
  auto remote = OpenRemote(repo, request.remote);
  RemoteCallbacks callbacks{&job, job.TakeCredential()};
  callbacks.PrepareRemote(remote.get(), false);
  callbacks.fetched = &outcome.updated_references;
  auto strings = Strings(request.refspecs);
  git_strarray specs{strings.data(), strings.size()};
  git_fetch_options options = GIT_FETCH_OPTIONS_INIT;
  ConfigureFetch(options, request, callbacks);
  outcome.objects = WriteEffect::Unknown;
  const int result = git_remote_fetch(remote.get(), strings.empty() ? nullptr : &specs, &options, "fetch");
  if (const auto* stats = git_remote_stats(remote.get()); stats && stats->received_objects) {
    outcome.objects = result == 0 ? WriteEffect::Changed : WriteEffect::Unknown;
  } else if (result == 0) {
    outcome.objects = WriteEffect::Unchanged;
  }
  callbacks.CheckResult(result, "Fetch from remote");
  if (tracking_ref.empty()) {
    return {};
  }
  // Pull uses the commit observed during this fetch. An unchanged ref has no update callback.
  const auto updated = std::ranges::find(outcome.updated_references, tracking_ref, &FetchedRef::name);
  return updated == outcome.updated_references.end() ? AdvertisedUpstream(remote.get(), request, tracking_ref)
                                                    : updated->target;
}

void Push(git_repository* repo, TransferBase& job, const PushRequest& request, PushOutcome& outcome) {
  RequireNoClientHooks(repo, {"pre-push"});
  auto remote = OpenRemote(repo, request.remote, true);
  std::vector<std::string> refs;
  for (const auto& item : request.updates) {
    RequireRefName(item.destination);
    if (item.policy == PushPolicy::ForceWithLease && LocalTransport(remote.get(), true)) {
      Fail(ErrorCode::Unsupported, "Leased force push requires a Smart transport");
    }
    if (std::ranges::any_of(outcome.references, [&](const auto& ref) { return ref.destination == item.destination; })) {
      Fail(ErrorCode::InvalidArgument, "Specify each push destination once");
    }
    if (item.expected) {
      Parse(*item.expected);
    }
    std::string spec = item.policy == PushPolicy::Normal ? "" : "+";
    if (item.source) {
      const auto oid = Parse(*item.source);
      auto object = Make<git_object, git_object_free>("Read push source", git_object_lookup, repo, &oid, GIT_OBJECT_ANY);
      git_object* commit = nullptr;
      if (git_object_peel(&commit, object.get(), GIT_OBJECT_COMMIT) == 0) {
        Object peeled(commit, git_object_free);
        RequireCheckoutFilters(repo, CommitTree(reinterpret_cast<git_commit*>(peeled.get())).get());
      }
      spec += Id(&oid).hex;
    }
    refs.push_back(std::move(spec) + ":" + item.destination);
    outcome.references.push_back({item.destination, RefPushStatus::NotAttempted, {}});
  }
  RemoteCallbacks callbacks{&job, job.TakeCredential()};
  callbacks.PrepareRemote(remote.get(), true);
  callbacks.push = &request;
  callbacks.push_result = &outcome;
  git_push_options options = GIT_PUSH_OPTIONS_INIT;
  options.pb_parallelism = 1;
  options.callbacks = callbacks.Bind();
  if (callbacks.BlockOffsiteRedirects()) options.follow_redirects = GIT_REMOTE_REDIRECT_NONE;
  auto strings = Strings(refs);
  git_strarray specs{strings.data(), strings.size()};
  const int result = git_remote_push(remote.get(), &specs, &options);
  if (result != 0 && job.may_have_pushed) {
    for (auto& ref : outcome.references) {
      if (ref.status == RefPushStatus::NotAttempted) {
        ref.status = RefPushStatus::Unknown;
      }
    }
  }
  callbacks.CheckResult(result, "Push to remote");
  if (std::ranges::any_of(outcome.references, [](const auto& ref) { return ref.status == RefPushStatus::Rejected; })) {
    outcome.report = Failed({ErrorCode::Git, "The remote rejected one or more reference updates"});
  }
}

void SubmoduleTransfer(git_repository* repo, TransferBase& job, const std::vector<std::string>& names,
                       bool initialize, bool clone, ChangeOutcome& outcome) {
  if (Operation(repo)) {
    Fail(ErrorCode::Busy, "Finish the current Git operation before updating submodules");
  }
  RemoteCallbacks callbacks{&job, job.TakeCredential()};
  for (const auto& name : names) {
    if (job.StopRequested()) {
      outcome.report = Stopped();
      return;
    }
    RequireString(name, "Submodule name");
    auto module = Make<git_submodule, git_submodule_free>("Find submodule", git_submodule_lookup, repo, name.c_str());
    const auto* url = git_submodule_url(module.get());
    ValidateTransferUrl(url ? url : "");
    callbacks.allow_credentials = !std::string_view(url).starts_with("http://");
    git_submodule_update_options options = GIT_SUBMODULE_UPDATE_OPTIONS_INIT;
    options.fetch_opts.callbacks = callbacks.Bind();
    options.checkout_opts.checkout_strategy = clone ? GIT_CHECKOUT_NONE : GIT_CHECKOUT_SAFE;
    outcome.worktree = WriteEffect::Unknown;
    if (clone) {
      git_repository* value = nullptr;
      const int code = git_submodule_clone(&value, module.get(), &options);
      Repo child(value, git_repository_free);
      callbacks.CheckResult(code, "Clone submodule");
      auto tree = HeadTree(child.get());
      RequireCheckoutFilters(child.get(), tree.get());
      git_checkout_options checkout = GIT_CHECKOUT_OPTIONS_INIT;
      checkout.checkout_strategy = GIT_CHECKOUT_SAFE;
      Check(git_checkout_head(child.get(), &checkout), "Check out cloned submodule");
    } else {
      const int code = git_submodule_update(module.get(), initialize ? 1 : 0, &options);
      callbacks.CheckResult(code, "Update submodule");
    }
    outcome.paths.emplace_back(git_submodule_path(module.get()));
    outcome.worktree = WriteEffect::Changed;
  }
}

struct CloneDirectory {
  fs::path staging;
  ~CloneDirectory() {
    std::error_code error;
    if (!staging.empty()) {
      fs::remove_all(staging, error);
    }
  }
};

Repo Clone(TransferBase& job, const CloneRequest& request, const File& destination) {
  static std::atomic<std::uint64_t> suffix{0};
  const auto path = Utf8Path(destination.Path());
  std::error_code error;
  if (fs::exists(path, error) || error) {
    Fail(ErrorCode::AlreadyExists, "Choose a new directory for clone");
  }
  CloneDirectory directory;
  for (int times = 0; times < 32; ++times) {
    auto candidate = path.parent_path() / (".huxerui-clone-" + std::to_string(++suffix));
    if (fs::create_directory(candidate, error)) {
      directory.staging = std::move(candidate);
      break;
    }
    if (error) {
      Fail(ErrorCode::Io, "Reserve clone destination: " + error.message());
    }
  }
  if (directory.staging.empty()) {
    Fail(ErrorCode::Busy, "Could not reserve a directory for clone");
  }
  RemoteCallbacks callbacks{&job, job.TakeCredential()};
  git_clone_options options = GIT_CLONE_OPTIONS_INIT;
  if (callbacks.RetryUrl(request.url)) {
    // Create origin with the requested URL and override only the connection for this attempt.
    options.remote_cb = [](git_remote** out, git_repository* repo, const char* name, const char* url, void* payload) {
      const auto& self = *static_cast<const RemoteCallbacks*>(payload);
      const int code = git_remote_create(out, repo, name, url);
      return code < 0 ? code : git_remote_set_instance_url(*out, self.credential->endpoint.c_str());
    };
    options.remote_cb_payload = &callbacks;
  }
  options.bare = request.bare;
  options.checkout_opts.checkout_strategy = GIT_CHECKOUT_NONE;
  options.checkout_branch = request.branch ? request.branch->c_str() : nullptr;
  options.local = request.depth ? GIT_CLONE_NO_LOCAL : GIT_CLONE_LOCAL_NO_LINKS;
  FetchRequest fetch;
  fetch.depth = request.depth;
  ConfigureFetch(options.fetch_opts, fetch, callbacks);
  git_repository* value = nullptr;
  const int code = git_clone(&value, request.url.c_str(), PathString(directory.staging).c_str(), &options);
  Repo repo(value, git_repository_free);
  callbacks.CheckResult(code, "Clone repository");
  if (job.StopRequested()) {
    Fail(ErrorCode::Cancelled, "Clone stopped");
  }
  if (!request.bare) {
    job.Progress(TransferPhase::CheckingOut);
    RequireCheckoutFilters(repo.get(), HeadTree(repo.get()).get());
    git_checkout_options checkout = GIT_CHECKOUT_OPTIONS_INIT;
    checkout.checkout_strategy = GIT_CHECKOUT_SAFE;
    const int checkout_code = git_checkout_head(repo.get(), &checkout);
    if (checkout_code != GIT_EUNBORNBRANCH) {
      Check(checkout_code, "Check out cloned repository");
    }
  }
  if (job.StopRequested()) {
    Fail(ErrorCode::Cancelled, "Clone stopped");
  }
  repo.reset();
  if (fs::exists(path, error) || error) {
    Fail(ErrorCode::AlreadyExists, "Clone destination appeared during transfer");
  }
  fs::rename(directory.staging, path, error);
  if (error) {
    Fail(ErrorCode::Io, "Publish cloned repository: " + error.message());
  }
  directory.staging.clear();
  return Make<git_repository, git_repository_free>("Reopen cloned repository", git_repository_open,
                                                    destination.Path().c_str());
}

} // namespace
} // namespace huxerui::git::detail

namespace huxerui::git {
using namespace detail;

TransferSubscription::TransferSubscription(std::shared_ptr<Observer> observer) : observer_(std::move(observer)) {}

TransferSubscription::TransferSubscription(TransferSubscription&&) noexcept = default;

TransferSubscription& TransferSubscription::operator=(TransferSubscription&& other) noexcept {
  if (this != &other) {
    if (observer_) observer_->Stop();
    observer_ = std::move(other.observer_);
  }
  return *this;
}

TransferSubscription::~TransferSubscription() {
  if (observer_) observer_->Stop();
}

template <class Outcome> TransferSnapshot Transfer<Outcome>::Snapshot() const {
  return state_->Snapshot();
}

template <class Outcome> Task<Outcome> Transfer<Outcome>::WaitAsync() const {
  return AwaitCallback<Outcome>([state = state_](CallbackCompletion<Outcome> completion) -> std::function<void()> {
    std::unique_lock lock(state->mutex);
    if (state->result) {
      lock.unlock();
      completion.Complete(*state->result);
      return {};
    }
    const auto id = ++state->next_waiter;
    state->waiters.emplace(id, completion);
    return [state, id] {
      std::scoped_lock lock(state->mutex);
      state->waiters.erase(id);
    };
  });
}

template <class Outcome>
TransferSubscription Transfer<Outcome>::Observe(TaskScope scope, std::function<void(TransferSnapshot)> observer) const {
  return TransferSubscription(state_->Observe(std::move(scope), std::move(observer)));
}

template <class Outcome> bool Transfer<Outcome>::RequestStop() const noexcept {
  return state_->RequestStop();
}

template <class Outcome>
GitResult<void> Transfer<Outcome>::AnswerAuth(std::uint64_t id, std::optional<Credential> credential) const {
  return state_->Answer(id, std::move(credential));
}

GitService::GitService(CredentialProvider provider) {
  if (provider) credentials_ = std::make_shared<const CredentialProvider>(std::move(provider));
}

GitResult<Transfer<CloneOutcome>> GitService::StartClone(CloneRequest request) const {
  try {
    ValidateTransferUrl(request.url);
    if (!std::holds_alternative<File>(request.destination)) {
      Fail(ErrorCode::Unsupported, "Choose an application-accessible local destination for clone");
    }
    if (request.branch) {
      RequireString(*request.branch, "Clone branch");
    }
    auto state = Start<CloneOutcome>(nullptr, [request = std::move(request)](auto& job, auto& outcome) {
      auto repo = Clone(job, request, std::get<File>(request.destination));
      outcome.repository = Repository(BindRepository(repo.get(), {}));
    }, credentials_, {.operation = CredentialOperation::Clone});
    return GitResult<Transfer<CloneOutcome>>::Success(Transfer<CloneOutcome>(std::move(state)));
  } catch (const Failure& error) {
    return GitResult<Transfer<CloneOutcome>>::Failure(error.error);
  }
}

GitResult<Transfer<FetchOutcome>> GitService::StartFetch(Repository repository, FetchRequest request) const {
  try {
    auto state = repository.state_;
    const CredentialRequest auth{CredentialOperation::Fetch, state->location.git_directory, request.remote};
    auto job = Start<FetchOutcome>(state->group, [state, request = std::move(request)](auto& job, auto& outcome) {
      auto repo = Open(*state, true);
      Fetch(repo.get(), job, request, outcome);
    }, credentials_, auth);
    return GitResult<Transfer<FetchOutcome>>::Success(Transfer<FetchOutcome>(std::move(job)));
  } catch (const Failure& error) {
    return GitResult<Transfer<FetchOutcome>>::Failure(error.error);
  }
}

GitResult<Transfer<PushOutcome>> GitService::StartPush(Repository repository, PushRequest request) const {
  try {
    if (request.updates.empty()) {
      Fail(ErrorCode::InvalidArgument, "Select one or more push destinations");
    }
    auto state = repository.state_;
    const CredentialRequest auth{CredentialOperation::Push, state->location.git_directory, request.remote};
    auto job = Start<PushOutcome>(state->group, [state, request = std::move(request)](auto& job, auto& outcome) {
      auto repo = Open(*state, true);
      Push(repo.get(), job, request, outcome);
    }, credentials_, auth);
    return GitResult<Transfer<PushOutcome>>::Success(Transfer<PushOutcome>(std::move(job)));
  } catch (const Failure& error) {
    return GitResult<Transfer<PushOutcome>>::Failure(error.error);
  }
}

GitResult<Transfer<PullOutcome>> GitService::StartPull(Repository repository, PullRequest request) const {
  try {
    auto state = repository.state_;
    const CredentialRequest auth{CredentialOperation::Pull, state->location.git_directory, request.fetch.remote};
    auto job = Start<PullOutcome>(state->group, [state, request = std::move(request)](auto& job, auto& outcome) {
      auto repo = Open(*state, true);
      if (ReadHead(repo.get()) != request.expected) {
        Fail(ErrorCode::StaleSnapshot, "HEAD changed before pull started");
      }
      if (Operation(repo.get())) {
        Fail(ErrorCode::Busy, "Finish the current Git operation before pulling");
      }
      RequireRefName(request.upstream_ref);
      auto& fetch = outcome.fetch.emplace();
      std::optional<ObjectId> target;
      try {
        target = Fetch(repo.get(), job, request.fetch, fetch, request.upstream_ref);
      } catch (const Failure& error) {
        fetch.report = Failed(error.error);
        throw;
      }
      if (job.StopRequested()) {
        outcome.report = Stopped();
        return;
      }
      auto& integration = outcome.integration.emplace();
      try {
        if (ReadHead(repo.get()) != request.expected) {
          Fail(ErrorCode::StaleSnapshot, "HEAD changed after fetch");
        }
        if (!target) {
          Fail(ErrorCode::NoUpstream, "The selected upstream was not part of this fetch");
        }
        integration = Integrate(repo.get(), *target, request.mode, job.stop.get_token(), request.expected);
        outcome.report = integration.report;
      } catch (const Failure& error) {
        integration.report = Failed(error.error);
        throw;
      }
    }, credentials_, auth);
    return GitResult<Transfer<PullOutcome>>::Success(Transfer<PullOutcome>(std::move(job)));
  } catch (const Failure& error) {
    return GitResult<Transfer<PullOutcome>>::Failure(error.error);
  }
}

GitResult<Transfer<ChangeOutcome>> GitService::StartLfs(Repository repository, LfsRequest request,
                                                        GitCliOptions options) const {
  try {
    if (!GitCliSupported()) {
      Fail(ErrorCode::Unsupported, "This platform does not provide a Git executable backend");
    }
    auto state = repository.state_;
    auto job = Start<ChangeOutcome>(state->group,
        [state, request = std::move(request), options = std::move(options)](auto& job, auto& outcome) {
          outcome.report.backend = Backend::GitCli;
          auto repo = Open(*state, true);
          RunLfs(repo.get(), request, options, job.stop.get_token(), outcome);
        });
    return GitResult<Transfer<ChangeOutcome>>::Success(Transfer<ChangeOutcome>(std::move(job)));
  } catch (const Failure& error) {
    return GitResult<Transfer<ChangeOutcome>>::Failure(error.error);
  }
}

GitResult<Transfer<ChangeOutcome>> GitService::StartCloneSubmodule(Repository repository, std::string name) const {
  try {
    RequireString(name, "Submodule name");
    auto state = repository.state_;
    auto job = Start<ChangeOutcome>(state->group, [state, name = std::move(name)](auto& job, auto& outcome) {
      auto repo = Open(*state, true);
      SubmoduleTransfer(repo.get(), job, {name}, true, true, outcome);
    });
    return GitResult<Transfer<ChangeOutcome>>::Success(Transfer<ChangeOutcome>(std::move(job)));
  } catch (const Failure& error) {
    return GitResult<Transfer<ChangeOutcome>>::Failure(error.error);
  }
}

GitResult<Transfer<ChangeOutcome>> GitService::StartUpdateSubmodules(Repository repository,
                                                                     std::vector<std::string> names,
                                                                     bool initialize) const {
  try {
    if (names.empty()) {
      Fail(ErrorCode::InvalidArgument, "Select submodules to update");
    }
    auto state = repository.state_;
    auto job = Start<ChangeOutcome>(state->group,
        [state, names = std::move(names), initialize](auto& job, auto& outcome) {
          auto repo = Open(*state, true);
          SubmoduleTransfer(repo.get(), job, names, initialize, false, outcome);
        });
    return GitResult<Transfer<ChangeOutcome>>::Success(Transfer<ChangeOutcome>(std::move(job)));
  } catch (const Failure& error) {
    return GitResult<Transfer<ChangeOutcome>>::Failure(error.error);
  }
}

Task<GitResult<std::vector<RefInfo>>> GitService::ReadRemoteRefsAsync(Repository handle, std::string name,
                                                                      std::optional<Credential> credential) const {
  auto state = handle.state_;
  return Dispatch<GitResult<std::vector<RefInfo>>>(nullptr,
      [state, name = std::move(name), credential = std::move(credential)](std::stop_token) mutable {
        try {
          auto repo = Open(*state);
          auto remote = OpenRemote(repo.get(), name);
          RemoteCallbacks context{nullptr, credential
              ? std::optional(ScopedCredential{std::move(*credential), RedactUrl(git_remote_url(remote.get()))})
              : std::nullopt};
          context.allow_credentials = !std::string_view(git_remote_url(remote.get())).starts_with("http://");
          const auto callbacks = context.Bind();
          context.CheckResult(git_remote_connect(remote.get(), GIT_DIRECTION_FETCH, &callbacks, nullptr, nullptr),
                              "Read remote advertisement");
          const git_remote_head** heads = nullptr;
          std::size_t count = 0;
          context.CheckResult(git_remote_ls(&heads, &count, remote.get()), "List remote references");
          std::vector<RefInfo> refs;
          for (std::size_t i = 0; i < count; ++i) {
            refs.push_back({heads[i]->name, OptionalId(&heads[i]->oid), {},
                            heads[i]->symref_target ? heads[i]->symref_target : ""});
          }
          git_remote_disconnect(remote.get());
          return GitResult<std::vector<RefInfo>>::Success(std::move(refs));
        } catch (const Failure& error) {
          return GitResult<std::vector<RefInfo>>::Failure(error.error);
        }
      }, true);
}

template class Transfer<CloneOutcome>;
template class Transfer<FetchOutcome>;
template class Transfer<PushOutcome>;
template class Transfer<PullOutcome>;
template class Transfer<ChangeOutcome>;

} // namespace huxerui::git
