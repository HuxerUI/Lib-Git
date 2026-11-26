#include "git_internal.h"

#include <algorithm>
#include <map>

namespace huxerui::git::detail {
namespace {

using Submodule = Handle<git_submodule, git_submodule_free>;
using Worktree = Handle<git_worktree, git_worktree_free>;

struct Strings {
  git_strarray value{};
  ~Strings() { git_strarray_dispose(&value); }
};

Ref LocalBranch(git_repository* repo, std::string_view input) {
  const std::string name(input.starts_with("refs/heads/") ? input.substr(11) : input);
  RequireString(name, "Branch name");
  return Make<git_reference, git_reference_free>("Read local branch", git_branch_lookup, repo,
                                                 name.c_str(), GIT_BRANCH_LOCAL);
}

std::optional<std::string> Upstream(git_repository* repo, const char* ref) {
  git_buf buffer = GIT_BUF_INIT;
  const int code = git_branch_upstream_name(&buffer, repo, ref);
  std::optional<std::string> name;
  if (code == 0) {
    name = std::string(buffer.ptr, buffer.size);
  }
  git_buf_dispose(&buffer);
  if (code != GIT_ENOTFOUND) {
    Check(code, "Read branch upstream");
  }
  return name;
}

std::vector<RefInfo> Refs(git_repository* repo, bool tags) {
  Handle<git_reference_iterator, git_reference_iterator_free> refs(nullptr, git_reference_iterator_free);
  if (tags) {
    refs = Make<git_reference_iterator, git_reference_iterator_free>("List tags", git_reference_iterator_glob_new,
                                                                     repo, "refs/tags/*");
  } else {
    refs = Make<git_reference_iterator, git_reference_iterator_free>("List refs", git_reference_iterator_new, repo);
  }
  std::vector<RefInfo> result;
  for (;;) {
    git_reference* value = nullptr;
    const int code = git_reference_next(&value, refs.get());
    Ref ref(value, git_reference_free);
    if (code == GIT_ITEROVER) {
      break;
    }
    Check(code, "Read reference");
    RefInfo item{git_reference_name(ref.get()), OptionalId(git_reference_target(ref.get())), {}, {}};
    if (const auto* name = git_reference_symbolic_target(ref.get())) {
      item.symbolic_target = name;
    }
    if (tags) {
      git_object* object = nullptr;
      const int peel = git_reference_peel(&object, ref.get(), GIT_OBJECT_ANY);
      Object peeled(object, git_object_free);
      if (peel == 0) {
        item.peeled = Id(git_object_id(peeled.get()));
      }
    }
    result.push_back(std::move(item));
  }
  std::ranges::sort(result, {}, &RefInfo::name);
  return result;
}

std::vector<StashInfo> Stashes(git_repository* repo) {
  std::vector<StashInfo> entries;
  std::exception_ptr failure;
  struct List { std::vector<StashInfo>& entries; std::exception_ptr& failure; } result{entries, failure};
  const int code = git_stash_foreach(repo, [](std::size_t index, const char* message, const git_oid* id,
                                               void* payload) -> int {
    auto& list = *static_cast<List*>(payload);
    try {
      list.entries.push_back({index, Id(id), message ? message : ""});
      return 0;
    } catch (...) {
      list.failure = std::current_exception();
      return GIT_EUSER;
    }
  }, &result);
  if (failure) {
    std::rethrow_exception(failure);
  }
  Check(code, "List stashes");
  return entries;
}

std::size_t CheckStash(git_repository* repo, const StashInfo& stash) {
  const auto entries = Stashes(repo);
  if (stash.index >= entries.size() || entries[stash.index].id != stash.id) {
    Fail(ErrorCode::StaleSnapshot, "The selected stash moved. Refresh the stash list");
  }
  return stash.index;
}

Submodule FindSubmodule(git_repository* repo, const std::string& name) {
  RequireString(name, "Submodule name");
  return Make<git_submodule, git_submodule_free>("Read submodule", git_submodule_lookup, repo, name.c_str());
}

Worktree FindWorktree(git_repository* repo, const std::string& name) {
  ValidatePath(name);
  if (name.find('/') != std::string::npos) {
    Fail(ErrorCode::InvalidArgument, "A worktree name must be one path component");
  }
  return Make<git_worktree, git_worktree_free>("Read worktree", git_worktree_lookup, repo, name.c_str());
}

void MatchRef(const git_reference* ref, const ObjectId& expected) {
  const auto* current = git_reference_target(ref);
  if (!current || Id(current) != expected) {
    Fail(ErrorCode::StaleSnapshot, "The reference changed. Refresh it before editing");
  }
}

/// Prepares a move of one branch's local configuration while its configuration transaction is held.
/// @param config Locked local configuration.
/// @param source Old branch shorthand.
/// @param destination New branch shorthand.
/// @return Whether there are settings to publish.
bool PrepareBranchConfigRename(git_config* config, std::string_view source, const std::string& destination) {
  const auto section = "branch." + std::string(source);
  auto iterator = Make<git_config_iterator, git_config_iterator_free>("Read branch configuration",
                                                                      git_config_iterator_new, config);
  std::map<std::string, std::vector<std::string>> settings;
  for (;;) {
    git_config_entry* entry = nullptr;
    const auto code = git_config_next(&entry, iterator.get());
    if (code == GIT_ITEROVER) {
      break;
    }
    Check(code, "Read branch setting");
    const std::string_view key(entry->name);
    if (key.substr(0, key.rfind('.')) == section) {
      settings[entry->name].push_back(entry->value ? entry->value : "true");
    }
  }
  for (const auto& [key, values] : settings) {
    const auto target = "branch." + destination + key.substr(section.size());
    for (const auto& value : values) {
      // A pattern that cannot match appends the value, including an empty one.
      Check(git_config_set_multivar(config, target.c_str(), "a^", value.c_str()), "Move branch setting");
    }
    Check(git_config_delete_multivar(config, key.c_str(), ".*"), "Remove previous branch setting");
  }
  return !settings.empty();
}

/// Preserves the old branch's log and records the rename in the new log.
/// @param repo Repository containing both branch references.
/// @param source Old full reference name.
/// @param destination New full reference name.
/// @param tip Commit at the tip of both branches.
/// @param message Reflog message for the rename.
void MoveBranchReflog(git_repository* repo, const char* source, const char* destination,
                      const git_oid* tip, const char* message) {
  const int exists = git_reference_has_log(repo, source);
  Check(exists, "Inspect original branch reflog");
  if (!exists) {
    return;
  }
  git_signature* identity = nullptr;
  const int code = git_signature_default(&identity, repo);
  Handle<git_signature, git_signature_free> signature(identity, git_signature_free);
  if (code == GIT_ENOTFOUND) {
    signature = Make<git_signature, git_signature_free>("Create branch reflog identity", git_signature_now,
                                                        "unknown", "unknown");
  } else {
    Check(code, "Read branch reflog identity");
  }
  Check(git_reflog_rename(repo, source, destination), "Move branch reflog");
  auto log = Make<git_reflog, git_reflog_free>("Read moved branch reflog", git_reflog_read, repo, destination);
  Check(git_reflog_append(log.get(), tip, signature.get(), message), "Record branch rename");
  Check(git_reflog_write(log.get()), "Publish branch reflog");
}

void RequireIdle(git_repository* repo) {
  if (Operation(repo)) {
    Fail(ErrorCode::Busy, "Finish the current Git operation first");
  }
}

} // namespace
} // namespace huxerui::git::detail

namespace huxerui::git {
using namespace detail;

Task<GitResult<std::vector<RefInfo>>> Repository::ListRefsAsync() const {
  return Query(state_, [](auto* repo, std::stop_token) { return Refs(repo, false); });
}

Task<GitResult<ObjectId>> Repository::ResolveRevisionAsync(std::string name) const {
  return Query(state_, [name = std::move(name)](auto* repo, std::stop_token) {
    return Id(git_object_id(Revision(repo, name).get()));
  });
}

Task<GitResult<Reachability>> Repository::CompareReachabilityAsync(ObjectId local, ObjectId upstream) const {
  return Query(state_, [local = std::move(local), upstream = std::move(upstream)](auto* repo, std::stop_token) {
    const auto lhs = Parse(local);
    const auto rhs = Parse(upstream);
    Reachability result;
    Check(git_graph_ahead_behind(&result.ahead, &result.behind, repo, &lhs, &rhs), "Compare branch history");
    result.shallow = git_repository_is_shallow(repo) != 0;
    return result;
  }, true);
}

Task<GitResult<std::optional<Signature>>> Repository::ReadIdentityAsync() const {
  return Query(state_, [](auto* repo, std::stop_token) -> std::optional<Signature> {
    git_signature* value = nullptr;
    const int code = git_signature_default(&value, repo);
    Handle<git_signature, git_signature_free> signature(value, git_signature_free);
    if (code == GIT_ENOTFOUND) {
      return {};
    }
    Check(code, "Read configured identity");
    return ReadSignature(signature.get());
  });
}

Task<ChangeOutcome> Repository::SetIdentityAsync(Signature identity) const {
  return Write<ChangeOutcome>(state_, [identity = std::move(identity)](auto* repo, auto&, std::stop_token) {
    auto validated = MakeSignature(repo, identity);
    auto config = Make<git_config, git_config_free>("Open Git configuration", git_repository_config, repo);
    auto local = Make<git_config, git_config_free>("Open repository configuration", git_config_open_level,
                                                   config.get(), GIT_CONFIG_LEVEL_LOCAL);
    auto transaction = Make<git_transaction, git_transaction_free>("Lock repository configuration",
                                                                     git_config_lock, local.get());
    Check(git_config_set_string(local.get(), "user.name", validated->name), "Set the repository author name");
    Check(git_config_set_string(local.get(), "user.email", validated->email), "Set the repository author email");
    Check(git_transaction_commit(transaction.get()), "Publish repository identity");
  });
}

Task<GitResult<std::vector<BranchInfo>>> Repository::ListBranchesAsync() const {
  return Query(state_, [](auto* repo, std::stop_token) {
    auto branches = Make<git_branch_iterator, git_branch_iterator_free>("List branches", git_branch_iterator_new,
                                                                        repo, GIT_BRANCH_ALL);
    std::vector<BranchInfo> list;
    for (;;) {
      git_reference* value = nullptr;
      git_branch_t kind;
      const auto code = git_branch_next(&value, &kind, branches.get());
      Ref branch(value, git_reference_free);
      if (code == GIT_ITEROVER) {
        break;
      }
      Check(code, "Read branch");
      if (const auto* oid = git_reference_target(branch.get())) {
        const auto* name = git_reference_name(branch.get());
        const bool remote = kind == GIT_BRANCH_REMOTE;
        list.push_back({name, Id(oid), remote ? std::nullopt : Upstream(repo, name), remote,
                        !remote && git_branch_is_head(branch.get()) == 1,
                        !remote && git_branch_is_checked_out(branch.get()) == 1});
      }
    }
    std::ranges::sort(list, {}, &BranchInfo::name);
    return list;
  });
}

Task<ChangeOutcome> Repository::CreateBranchAsync(std::string name, ObjectId target) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name), target = std::move(target)]
      (auto* repo, auto& result, std::stop_token) {
    RequireString(name, "Branch name");
    auto commit = FindCommit(repo, target);
    Make<git_reference, git_reference_free>("Create branch", git_branch_create, repo, name.c_str(), commit.get(), 0);
    result.references = WriteEffect::Changed;
  });
}

Task<ChangeOutcome> Repository::RenameBranchAsync(std::string name, std::string replacement, ObjectId expected) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name), replacement = std::move(replacement),
                                      expected = std::move(expected)](auto* repo, auto& result, std::stop_token) {
    RequireString(replacement, "New branch name");
    const auto shorthand = name.starts_with("refs/heads/") ? name.substr(11) : name;
    RequireString(shorthand, "Branch name");
    const auto source = "refs/heads/" + shorthand;
    const auto destination = "refs/heads/" + replacement;
    RequireRefName(source);
    RequireRefName(destination);
    if (source == destination) {
      Fail(ErrorCode::AlreadyExists, "The branch already has this name");
    }
    if (source.starts_with(destination + "/") || destination.starts_with(source + "/")) {
      Fail(ErrorCode::Unsupported, "The old and new branch names must be able to coexist during a rename");
    }

    auto head_lock = Make<git_transaction, git_transaction_free>("Prepare HEAD update", git_transaction_new, repo);
    Check(git_transaction_lock_ref(head_lock.get(), "HEAD"), "Lock HEAD for branch rename");
    auto source_lock = Make<git_transaction, git_transaction_free>("Prepare branch removal", git_transaction_new, repo);
    Check(git_transaction_lock_ref(source_lock.get(), source.c_str()), "Lock original branch");
    auto branch = LocalBranch(repo, shorthand);
    MatchRef(branch.get(), expected);
    const auto current = git_branch_is_head(branch.get());
    Check(current, "Inspect current branch");
    if (!current && git_branch_is_checked_out(branch.get()) == 1) {
      Fail(ErrorCode::Busy, "The branch is checked out in another worktree");
    }
    auto config = Make<git_config, git_config_free>("Open Git configuration", git_repository_config, repo);
    auto local = Make<git_config, git_config_free>("Open local branch configuration", git_config_open_level,
                                                   config.get(), GIT_CONFIG_LEVEL_LOCAL);
    auto config_lock = Make<git_transaction, git_transaction_free>("Lock branch configuration", git_config_lock, local.get());
    const auto settings_moved = PrepareBranchConfigRename(local.get(), shorthand, replacement);

    const auto message = "branch: renamed " + source + " to " + destination;
    const auto* tip = git_reference_target(branch.get());
    const git_oid absent{};
    // Publish and lock the destination before removing the source. Each reference transaction writes independently.
    Make<git_reference, git_reference_free>("Create replacement branch", git_reference_create_matching, repo,
                                            destination.c_str(), tip, 0, &absent, message.c_str());
    result.references = WriteEffect::Changed;
    auto destination_lock = Make<git_transaction, git_transaction_free>("Protect replacement branch", git_transaction_new, repo);
    Check(git_transaction_lock_ref(destination_lock.get(), destination.c_str()), "Lock replacement branch");
    auto replacement_ref = LocalBranch(repo, replacement);
    MatchRef(replacement_ref.get(), expected);
    MoveBranchReflog(repo, source.c_str(), destination.c_str(), tip, message.c_str());

    if (current) {
      Check(git_transaction_set_symbolic_target(head_lock.get(), "HEAD", destination.c_str(), nullptr, message.c_str()),
            "Prepare renamed HEAD");
      Check(git_transaction_commit(head_lock.get()), "Point HEAD to the renamed branch");
    } else if (git_branch_is_checked_out(branch.get()) == 1) {
      Fail(ErrorCode::Busy, "The original branch was checked out during the rename");
    }
    Check(git_transaction_remove(source_lock.get(), source.c_str()), "Prepare original branch removal");
    Check(git_transaction_commit(source_lock.get()), "Remove original branch");
    if (settings_moved) {
      Check(git_transaction_commit(config_lock.get()), "Publish renamed branch configuration");
    }
  });
}

Task<ChangeOutcome> Repository::DeleteBranchAsync(std::string name, ObjectId expected) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name), expected = std::move(expected)]
      (auto* repo, auto& result, std::stop_token) {
    auto branch = LocalBranch(repo, name);
    MatchRef(branch.get(), expected);
    if (git_branch_is_checked_out(branch.get()) == 1) {
      Fail(ErrorCode::Busy, "The branch is checked out in a worktree");
    }
    Check(git_branch_delete(branch.get()), "Delete branch");
    result.references = WriteEffect::Changed;
  });
}

Task<ChangeOutcome> Repository::SetUpstreamAsync(std::string name, std::optional<std::string> upstream) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name), upstream = std::move(upstream)]
      (auto* repo, auto&, std::stop_token) {
    auto branch = LocalBranch(repo, name);
    if (upstream) {
      RequireString(*upstream, "Upstream branch");
    }
    Check(git_branch_set_upstream(branch.get(), upstream ? upstream->c_str() : nullptr), "Set branch upstream");
  });
}

Task<GitResult<std::vector<RefInfo>>> Repository::ListTagsAsync() const {
  return Query(state_, [](auto* repo, std::stop_token) { return Refs(repo, true); });
}

Task<ChangeOutcome> Repository::CreateTagAsync(std::string name, ObjectId target,
                                               std::optional<std::string> message) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name), target = std::move(target), message = std::move(message)]
      (auto* repo, auto& result, std::stop_token) {
    RequireString(name, "Tag name");
    const auto oid = Parse(target);
    auto object = Make<git_object, git_object_free>("Read tag target", git_object_lookup, repo, &oid, GIT_OBJECT_ANY);
    git_oid tag{};
    if (message) {
      RequireString(*message, "Tag message");
      auto signature = MakeSignature(repo, {});
      Check(git_tag_create(&tag, repo, name.c_str(), object.get(), signature.get(), message->c_str(), 0),
            "Create annotated tag");
    } else {
      Check(git_tag_create_lightweight(&tag, repo, name.c_str(), object.get(), 0), "Create tag");
    }
    result.references = WriteEffect::Changed;
  });
}

Task<ChangeOutcome> Repository::DeleteTagAsync(std::string name, ObjectId expected) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name), expected = std::move(expected)]
      (auto* repo, auto& result, std::stop_token) {
    const auto full = name.starts_with("refs/tags/") ? name : "refs/tags/" + name;
    RequireRefName(full);
    auto tag = Make<git_reference, git_reference_free>("Read tag", git_reference_lookup, repo, full.c_str());
    MatchRef(tag.get(), expected);
    Check(git_reference_delete(tag.get()), "Delete tag");
    result.references = WriteEffect::Changed;
  });
}

Task<GitResult<std::vector<RemoteInfo>>> Repository::ListRemotesAsync() const {
  return Query(state_, [](auto* repo, std::stop_token) {
    Strings remotes;
    Check(git_remote_list(&remotes.value, repo), "List remotes");
    std::vector<RemoteInfo> result;
    for (std::size_t i = 0; i < remotes.value.count; ++i) {
      const auto* name = remotes.value.strings[i];
      auto remote = Make<git_remote, git_remote_free>("Read remote", git_remote_lookup, repo, name);
      const auto* url = git_remote_url(remote.get());
      const auto* push = git_remote_pushurl(remote.get());
      RemoteInfo info{name, RedactUrl(url ? url : ""), RedactUrl(push ? push : url ? url : ""), {}};
      Strings specs;
      Check(git_remote_get_fetch_refspecs(&specs.value, remote.get()), "Read fetch refspecs");
      for (std::size_t j = 0; j < specs.value.count; ++j) {
        info.fetch_refspecs.emplace_back(specs.value.strings[j]);
      }
      result.push_back(std::move(info));
    }
    return result;
  });
}

Task<ChangeOutcome> Repository::AddRemoteAsync(std::string name, std::string url) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name), url = std::move(url)]
      (auto* repo, auto&, std::stop_token) {
    RequireString(name, "Remote name");
    RequireString(url, "Remote address");
    if (RedactUrl(url) != url && !url.starts_with("ssh://")) {
      Fail(ErrorCode::InvalidArgument, "Supply credentials through authentication instead of the remote URL");
    }
    Make<git_remote, git_remote_free>("Add remote", git_remote_create, repo, name.c_str(), url.c_str());
  });
}

Task<ChangeOutcome> Repository::SetRemoteUrlAsync(std::string name, std::string url, bool push_url) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name), url = std::move(url), push_url]
      (auto* repo, auto&, std::stop_token) {
    RequireString(name, "Remote name");
    RequireString(url, "Remote address");
    if (RedactUrl(url) != url && !url.starts_with("ssh://")) {
      Fail(ErrorCode::InvalidArgument, "Supply credentials through authentication instead of the remote URL");
    }
    Check(push_url ? git_remote_set_pushurl(repo, name.c_str(), url.c_str())
                   : git_remote_set_url(repo, name.c_str(), url.c_str()), "Set remote address");
  });
}

Task<ChangeOutcome> Repository::RenameRemoteAsync(std::string name, std::string replacement) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name), replacement = std::move(replacement)]
      (auto* repo, auto& result, std::stop_token) {
    RequireString(name, "Remote name");
    RequireString(replacement, "New remote name");
    Strings untouched;
    Check(git_remote_rename(&untouched.value, repo, name.c_str(), replacement.c_str()), "Rename remote");
    result.references = WriteEffect::Changed;
    if (untouched.value.count) {
      result.report.issue = GitError{ErrorCode::Unsupported, "Review the remote's custom refspecs after renaming"};
    }
  });
}

Task<ChangeOutcome> Repository::DeleteRemoteAsync(std::string name) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name)](auto* repo, auto& result, std::stop_token) {
    RequireString(name, "Remote name");
    Check(git_remote_delete(repo, name.c_str()), "Delete remote");
    result.references = WriteEffect::Changed;
  });
}

Task<GitResult<std::vector<StashInfo>>> Repository::ListStashesAsync() const {
  return Query(state_, [](auto* repo, std::stop_token) { return Stashes(repo); });
}

Task<ChangeOutcome> Repository::SaveStashAsync(std::string message, bool include_untracked) const {
  return Write<ChangeOutcome>(state_, [message = std::move(message), include_untracked]
      (auto* repo, auto& result, std::stop_token) {
    RequireIdle(repo);
    auto signature = MakeSignature(repo, {});
    git_oid oid{};
    result.index = result.worktree = result.references = WriteEffect::Unknown;
    const auto code = git_stash_save(&oid, repo, signature.get(), message.empty() ? nullptr : message.c_str(),
                                      include_untracked ? GIT_STASH_INCLUDE_UNTRACKED : GIT_STASH_DEFAULT);
    if (code == GIT_ENOTFOUND) {
      result = {};
      result.report.status = WriteStatus::NoChange;
      return;
    }
    Check(code, "Save stash");
    result.index = result.worktree = result.references = WriteEffect::Changed;
  });
}

Task<ChangeOutcome> Repository::ApplyStashAsync(StashInfo stash, bool reinstate_index, bool pop) const {
  return Write<ChangeOutcome>(state_, [stash = std::move(stash), reinstate_index, pop]
      (auto* repo, auto& result, std::stop_token) {
    RequireIdle(repo);
    const auto index = CheckStash(repo, stash);
    git_stash_apply_options options = GIT_STASH_APPLY_OPTIONS_INIT;
    options.flags = reinstate_index ? GIT_STASH_APPLY_REINSTATE_INDEX : GIT_STASH_APPLY_DEFAULT;
    result.worktree = result.index = WriteEffect::Unknown;
    const int code = pop ? git_stash_pop(repo, index, &options) : git_stash_apply(repo, index, &options);
    if (code == GIT_ECONFLICT) {
      result.report = {WriteStatus::NeedsResolution, LastError(code, "Apply stash")};
      return;
    }
    Check(code, "Apply stash");
    result.worktree = WriteEffect::Changed;
    result.index = reinstate_index ? WriteEffect::Changed : WriteEffect::Unchanged;
    if (pop) result.references = WriteEffect::Changed;
  });
}

Task<ChangeOutcome> Repository::DropStashAsync(StashInfo stash) const {
  return Write<ChangeOutcome>(state_, [stash = std::move(stash)](auto* repo, auto& result, std::stop_token) {
    Check(git_stash_drop(repo, CheckStash(repo, stash)), "Drop stash");
    result.references = WriteEffect::Changed;
  });
}

Task<GitResult<std::vector<SubmoduleInfo>>> Repository::ListSubmodulesAsync() const {
  return Query(state_, [](auto* repo, std::stop_token) {
    std::vector<SubmoduleInfo> result;
    std::exception_ptr failure;
    struct List { std::vector<SubmoduleInfo>& result; std::exception_ptr& failure; } list{result, failure};
    const int code = git_submodule_foreach(repo, [](git_submodule* submodule, const char* name, void* data) -> int {
      auto& list = *static_cast<List*>(data);
      try {
        const auto* url = git_submodule_url(submodule);
        list.result.push_back({name, git_submodule_path(submodule), RedactUrl(url ? url : ""),
                               OptionalId(git_submodule_index_id(submodule)),
                               OptionalId(git_submodule_wd_id(submodule))});
        return 0;
      } catch (...) {
        list.failure = std::current_exception();
        return GIT_EUSER;
      }
    }, &list);
    if (failure) {
      std::rethrow_exception(failure);
    }
    Check(code, "List submodules");
    return result;
  });
}

Task<ChangeOutcome> Repository::InitSubmoduleAsync(std::string name) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name)](auto* repo, auto&, std::stop_token) {
    Check(git_submodule_init(FindSubmodule(repo, name).get(), 0), "Initialize submodule");
  });
}

Task<GitResult<Repository>> Repository::OpenSubmoduleAsync(std::string name) const {
  auto grant = state_->location.access;
  return Query(state_, [name = std::move(name), grant = std::move(grant)](auto* repo, std::stop_token) {
    auto submodule = FindSubmodule(repo, name);
    auto child = Make<git_repository, git_repository_free>("Open submodule", git_submodule_open, submodule.get());
    return Repository(BindRepository(child.get(), grant));
  });
}

Task<ChangeOutcome> Repository::AddSubmoduleAsync(std::string path, std::string url) const {
  return Write<ChangeOutcome>(state_, [path = std::move(path), url = std::move(url)](auto* repo, auto& result,
                                                                                 std::stop_token) {
    RequireIdle(repo);
    ValidatePath(path);
    RequireString(url, "Submodule address");
    if (RedactUrl(url) != url) {
      Fail(ErrorCode::InvalidArgument, "Supply credentials through authentication instead of the submodule URL");
    }
    Make<git_submodule, git_submodule_free>("Register submodule", git_submodule_add_setup,
                                           repo, url.c_str(), path.c_str(), 1);
    result.worktree = WriteEffect::Changed;
    result.paths.push_back(".gitmodules");
  });
}

Task<ChangeOutcome> Repository::FinalizeSubmoduleAsync(std::string name) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name)](auto* repo, auto& result, std::stop_token) {
    RequireIdle(repo);
    auto submodule = FindSubmodule(repo, name);
    Check(git_submodule_add_finalize(submodule.get()), "Stage submodule and .gitmodules");
    result.index = WriteEffect::Changed;
    result.paths = {git_submodule_path(submodule.get()), ".gitmodules"};
  });
}

Task<ChangeOutcome> Repository::SyncSubmoduleAsync(std::string name) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name)](auto* repo, auto&, std::stop_token) {
    Check(git_submodule_sync(FindSubmodule(repo, name).get()), "Synchronize submodule settings");
  });
}

Task<GitResult<std::vector<WorktreeInfo>>> Repository::ListWorktreesAsync() const {
  return Query(state_, [](auto* repo, std::stop_token) {
    Strings worktrees;
    Check(git_worktree_list(&worktrees.value, repo), "List linked worktrees");
    std::vector<WorktreeInfo> result;
    for (std::size_t i = 0; i < worktrees.value.count; ++i) {
      const std::string name(worktrees.value.strings[i]);
      auto item = FindWorktree(repo, name);
      const auto locked = git_worktree_is_locked(nullptr, item.get());
      Check(locked, "Inspect worktree lock");
      result.push_back({name, File(git_worktree_path(item.get())), locked != 0, git_worktree_validate(item.get()) == 0});
    }
    return result;
  });
}

Task<ChangeOutcome> Repository::AddWorktreeAsync(std::string name, File directory,
                                                std::optional<std::string> branch) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name), directory = std::move(directory),
                                       branch = std::move(branch)](auto* repo, auto& result, std::stop_token) {
    ValidatePath(name);
    if (name.find('/') != std::string::npos) {
      Fail(ErrorCode::InvalidArgument, "A worktree name must be one path component");
    }
    auto ref = branch ? LocalBranch(repo, *branch) : Ref(nullptr, git_reference_free);
    auto tree = ref ? CommitTree(FindCommit(repo, Id(git_reference_target(ref.get()))).get()) : HeadTree(repo);
    RequireCheckoutFilters(repo, tree.get());
    git_worktree_add_options options = GIT_WORKTREE_ADD_OPTIONS_INIT;
    options.ref = ref.get();
    const auto path = directory.Path();
    Make<git_worktree, git_worktree_free>("Add linked worktree", git_worktree_add, repo, name.c_str(), path.c_str(),
                                          &options);
    result.worktree = WriteEffect::Changed;
    result.references = ref ? WriteEffect::Unchanged : WriteEffect::Changed;
  });
}

Task<ChangeOutcome> Repository::SetWorktreeLockedAsync(std::string name, bool locked, std::string reason) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name), locked, reason = std::move(reason)]
      (auto* repo, auto&, std::stop_token) {
    auto tree = FindWorktree(repo, name);
    Check(locked ? git_worktree_lock(tree.get(), reason.empty() ? nullptr : reason.c_str())
                 : git_worktree_unlock(tree.get()), "Change linked worktree lock");
  });
}

Task<ChangeOutcome> Repository::PruneWorktreeAsync(std::string name, bool remove_directory) const {
  return Write<ChangeOutcome>(state_, [name = std::move(name), remove_directory](auto* repo, auto& result,
                                                                                std::stop_token) {
    auto tree = FindWorktree(repo, name);
    git_worktree_prune_options options = GIT_WORKTREE_PRUNE_OPTIONS_INIT;
    if (remove_directory && git_worktree_validate(tree.get()) == 0) {
      auto linked = Make<git_repository, git_repository_free>("Open linked worktree",
          git_repository_open_from_worktree, tree.get());
      RequireIdle(linked.get());
      git_status_options status_options = GIT_STATUS_OPTIONS_INIT;
      status_options.flags = GIT_STATUS_OPT_NO_REFRESH | GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_INCLUDE_IGNORED;
      auto status = Make<git_status_list, git_status_list_free>("Check worktree for local files",
                                                               git_status_list_new, linked.get(), &status_options);
      if (git_status_list_entrycount(status.get()) != 0) {
        Fail(ErrorCode::LocalChangesWouldBeOverwritten, "The linked worktree contains local changes or files");
      }
      options.flags = GIT_WORKTREE_PRUNE_VALID | GIT_WORKTREE_PRUNE_WORKING_TREE;
    }
    const int prunable = git_worktree_is_prunable(tree.get(), &options);
    Check(prunable, "Check whether the linked worktree can be pruned");
    if (!prunable) {
      Fail(ErrorCode::Busy, "The linked worktree is active or locked");
    }
    Check(git_worktree_prune(tree.get(), &options), "Prune linked worktree");
    result.worktree = WriteEffect::Changed;
  });
}

} // namespace huxerui::git
