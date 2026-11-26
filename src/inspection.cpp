#include "git_internal.h"

#include <algorithm>
#include <limits>

namespace huxerui::git::detail {

struct HistoryState {
  std::shared_ptr<RepositoryState> repository;
  Repo opened{nullptr, git_repository_free};
  Handle<git_revwalk, git_revwalk_free> walk{nullptr, git_revwalk_free};
  std::atomic<bool> in_use{false};
  std::mutex mutex;
  HistoryPage pending;
};

namespace {

CommitSummary Summarize(git_commit* commit) {
  CommitSummary summary;
  summary.id = Id(git_commit_id(commit));
  if (const auto* subject = git_commit_summary(commit)) {
    summary.subject = subject;
  }
  summary.author = ReadSignature(git_commit_author(commit));
  summary.committed_at = std::chrono::sys_seconds{std::chrono::seconds{git_commit_time(commit)}};
  for (unsigned i = 0; i < git_commit_parentcount(commit); ++i) {
    summary.parents.push_back(Id(git_commit_parent_id(commit, i)));
  }
  return summary;
}

Task<GitResult<HistoryPage>> NextHistoryPage(std::shared_ptr<HistoryState> state, std::size_t limit) {
  if (limit == 0 || limit > 4096) {
    co_return GitResult<HistoryPage>::Failure({ErrorCode::InvalidArgument, "History page size must be within 1–4096"});
  }
  if (state->in_use.exchange(true)) {
    co_return GitResult<HistoryPage>::Failure({ErrorCode::Busy, "This cursor already has an active read"});
  }
  struct ReadLease {
    HistoryState& state;
    ~ReadLease() { state.in_use = false; }
  } lease{*state};
  auto result = co_await Dispatch<GitResult<HistoryPage>>(nullptr,
      [state, limit](std::stop_token stop) {
        std::scoped_lock lock(state->mutex);
        try {
          auto& page = state->pending;
          while (!page.exhausted && page.commits.size() < limit && !stop.stop_requested()) {
            git_oid oid{};
            const int code = git_revwalk_next(&oid, state->walk.get());
            if (code == GIT_ITEROVER) {
              page.exhausted = true;
              break;
            }
            Check(code, "Advance commit history");
            page.commits.push_back(Summarize(FindCommit(state->opened.get(), Id(&oid)).get()));
          }
          HistoryPage delivered;
          const auto count = std::min(limit, page.commits.size());
          delivered.commits.assign(page.commits.begin(), page.commits.begin() + count);
          delivered.exhausted = page.exhausted && count == page.commits.size();
          delivered.shallow = page.shallow;
          return GitResult<HistoryPage>::Success(std::move(delivered));
        } catch (const Failure& error) {
          return GitResult<HistoryPage>::Failure(error.error);
        }
      });
  if (result.Succeeded()) {
    // The worker keeps a page until its waiting coroutine actually resumes.
    std::scoped_lock lock(state->mutex);
    auto& commits = state->pending.commits;
    commits.erase(commits.begin(), commits.begin() + result.Value().commits.size());
  }
  co_return result;
}

DeltaKind Kind(git_delta_t change) {
  switch (change) {
    case GIT_DELTA_ADDED: return DeltaKind::Added;
    case GIT_DELTA_DELETED: return DeltaKind::Deleted;
    case GIT_DELTA_MODIFIED: return DeltaKind::Modified;
    case GIT_DELTA_RENAMED: return DeltaKind::Renamed;
    case GIT_DELTA_COPIED: return DeltaKind::Copied;
    case GIT_DELTA_TYPECHANGE: return DeltaKind::TypeChanged;
    case GIT_DELTA_UNTRACKED: return DeltaKind::Untracked;
    case GIT_DELTA_CONFLICTED: return DeltaKind::Conflicted;
    default: return DeltaKind::Other;
  }
}

} // namespace

FileDelta ReadDelta(const git_diff_delta& delta) {
  return {delta.old_file.path ? delta.old_file.path : "", delta.new_file.path ? delta.new_file.path : "",
          Kind(delta.status), delta.old_file.mode, delta.new_file.mode, OptionalId(&delta.old_file.id),
          OptionalId(&delta.new_file.id), (delta.flags & GIT_DIFF_FLAG_BINARY) != 0};
}

Handle<git_diff, git_diff_free> MakeDiff(git_repository* repo, const DiffQuery& query) {
  git_diff_options options = GIT_DIFF_OPTIONS_INIT;
  options.flags = GIT_DIFF_INCLUDE_TYPECHANGE;
  std::vector<char*> paths;
  for (const auto& path : query.paths) {
    ValidatePath(path);
    paths.push_back(const_cast<char*>(path.c_str()));
  }
  if (!paths.empty()) {
    options.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH;
    options.pathspec = {paths.data(), paths.size()};
  }

  Handle<git_diff, git_diff_free> diff(nullptr, git_diff_free);
  switch (query.kind) {
    case DiffKind::Worktree: {
      auto index = ReadIndex(repo);
      options.flags |= GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS;
      diff = Make<git_diff, git_diff_free>("Compare worktree", git_diff_index_to_workdir, repo, index.get(), &options);
      break;
    }
    case DiffKind::Staged: {
      auto tree = HeadTree(repo);
      auto index = ReadIndex(repo);
      diff = Make<git_diff, git_diff_free>("Compare staged changes", git_diff_tree_to_index, repo, tree.get(),
                                          index.get(), &options);
      break;
    }
    case DiffKind::Commit: {
      if (!query.commit) {
        Fail(ErrorCode::InvalidArgument, "A committed comparison requires a commit ID");
      }
      auto commit = FindCommit(repo, *query.commit);
      auto target = CommitTree(commit.get());
      Tree base(nullptr, git_tree_free);
      if (query.base) {
        base = CommitTree(FindCommit(repo, *query.base).get());
      } else if (git_commit_parentcount(commit.get()) > 0) {
        if (query.parent_index >= git_commit_parentcount(commit.get())) {
          Fail(ErrorCode::InvalidArgument, "The selected commit parent does not exist");
        }
        auto parent = Make<git_commit, git_commit_free>("Read selected parent", git_commit_parent, commit.get(),
                                                       static_cast<unsigned>(query.parent_index));
        base = CommitTree(parent.get());
      } else if (query.parent_index != 0) {
        Fail(ErrorCode::InvalidArgument, "A root commit has no parents");
      }
      diff = Make<git_diff, git_diff_free>("Compare commits", git_diff_tree_to_tree, repo, base.get(), target.get(),
                                          &options);
      break;
    }
  }
  if (query.detect_renames) {
    git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT;
    find.flags = GIT_DIFF_FIND_RENAMES;
    Check(git_diff_find_similar(diff.get(), &find), "Find renamed files");
  }
  return diff;
}

} // namespace huxerui::git::detail

namespace huxerui::git {
using namespace detail;

Task<GitResult<RepositorySnapshot>> Repository::InspectAsync() const {
  auto location = state_->location;
  return Query(state_, [location = std::move(location)](git_repository* repo, std::stop_token) {
    auto head = ReadHead(repo);
    std::optional<std::string> upstream;
    if (head.kind == HeadKind::Attached) {
      git_buf name = GIT_BUF_INIT;
      const int result = git_branch_upstream_name(&name, repo, head.reference.c_str());
      if (result == 0) {
        upstream = std::string(name.ptr, name.size);
      }
      git_buf_dispose(&name);
      if (result != GIT_ENOTFOUND) {
        Check(result, "Read upstream branch");
      }
    }
    return RepositorySnapshot{location.git_directory, location.work_directory, std::move(head), std::move(upstream),
                              Operation(repo), git_repository_is_shallow(repo) != 0};
  });
}

Task<GitResult<StatusSnapshot>> Repository::StatusAsync(StatusQuery query) const {
  return Query(state_, [query](git_repository* repo, std::stop_token stop) {
    auto version = ReadVersion(repo);
    git_status_options options = GIT_STATUS_OPTIONS_INIT;
    options.flags = GIT_STATUS_OPT_NO_REFRESH | GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
        (query.include_untracked ? GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS : 0) |
        (query.include_ignored ? GIT_STATUS_OPT_INCLUDE_IGNORED : 0);
    auto status = Make<git_status_list, git_status_list_free>("Read repository status",
                                                             git_status_list_new, repo, &options);
    StatusSnapshot snapshot{version->head, CommitPrecondition(version), {}};
    for (std::size_t i = 0; i < git_status_list_entrycount(status.get()); ++i) {
      if (stop.stop_requested()) {
        Fail(ErrorCode::Cancelled, "Status read stopped");
      }
      const auto* entry = git_status_byindex(status.get(), i);
      const auto* delta = entry->index_to_workdir ? entry->index_to_workdir : entry->head_to_index;
      if (!delta) {
        continue;
      }
      const unsigned flags = entry->status;
      constexpr unsigned staged = GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED | GIT_STATUS_INDEX_DELETED |
                                  GIT_STATUS_INDEX_RENAMED | GIT_STATUS_INDEX_TYPECHANGE;
      constexpr unsigned unstaged = GIT_STATUS_WT_NEW | GIT_STATUS_WT_MODIFIED | GIT_STATUS_WT_DELETED |
                                    GIT_STATUS_WT_RENAMED | GIT_STATUS_WT_TYPECHANGE | GIT_STATUS_WT_UNREADABLE;
      snapshot.files.push_back({
          .path = delta->new_file.path ? delta->new_file.path : delta->old_file.path,
          .previous_path = delta->old_file.path ? delta->old_file.path : "",
          .staged = (flags & staged) != 0, .unstaged = (flags & unstaged) != 0,
          .untracked = (flags & GIT_STATUS_WT_NEW) != 0, .ignored = (flags & GIT_STATUS_IGNORED) != 0,
          .conflicted = (flags & GIT_STATUS_CONFLICTED) != 0,
          .deleted = (flags & (GIT_STATUS_INDEX_DELETED | GIT_STATUS_WT_DELETED)) != 0,
      });
    }
    if (!MatchesVersion(repo, *version)) {
      Fail(ErrorCode::StaleSnapshot, "HEAD or index changed while reading status");
    }
    return snapshot;
  });
}

Task<GitResult<HistoryCursor>> Repository::OpenHistoryAsync(HistoryQuery query) const {
  auto state = state_;
  return Dispatch<GitResult<HistoryCursor>>(state->group,
      [state, query = std::move(query)](std::stop_token) {
        try {
          auto cursor = std::make_shared<HistoryState>();
          cursor->repository = state;
          cursor->opened = Open(*state);
          auto* repo = cursor->opened.get();
          cursor->walk = Make<git_revwalk, git_revwalk_free>("Create history walker", git_revwalk_new, repo);
          Check(git_revwalk_sorting(cursor->walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME), "Order commit history");
          cursor->pending.shallow = git_repository_is_shallow(repo) != 0;
          for (const auto& id : query.starts) {
            const auto oid = Parse(id);
            Check(git_revwalk_push(cursor->walk.get(), &oid), "Select history start");
          }
          if (query.all_refs) {
            for (auto glob : {"refs/heads/*", "refs/remotes/*", "refs/tags/*"}) {
              const int result = git_revwalk_push_glob(cursor->walk.get(), glob);
              if (result != GIT_ENOTFOUND) {
                Check(result, "Select reference history");
              }
            }
          }
          if (query.starts.empty()) {
            const int result = git_revwalk_push_head(cursor->walk.get());
            if (result != GIT_EUNBORNBRANCH && result != GIT_ENOTFOUND) {
              Check(result, "Select HEAD history");
            }
          }
          return GitResult<HistoryCursor>::Success(HistoryCursor(std::move(cursor)));
        } catch (const Failure& error) {
          return GitResult<HistoryCursor>::Failure(error.error);
        }
      });
}

Task<GitResult<HistoryPage>> HistoryCursor::NextAsync(std::size_t limit) {
  return NextHistoryPage(state_, limit);
}

Task<GitResult<CommitDetails>> Repository::ReadCommitAsync(ObjectId id) const {
  return Query(state_, [id = std::move(id)](git_repository* repo, std::stop_token) {
    auto commit = FindCommit(repo, id);
    const auto* message = git_commit_message(commit.get());
    return CommitDetails{Summarize(commit.get()), Id(git_commit_tree_id(commit.get())), message ? message : "",
                         ReadSignature(git_commit_committer(commit.get()))};
  }, true);
}

Task<GitResult<std::optional<CommitSignature>>> Repository::ReadCommitSignatureAsync(ObjectId id) const {
  return Query(state_, [id = std::move(id)](auto* repo, std::stop_token) -> std::optional<CommitSignature> {
    auto oid = Parse(id);
    struct Buffer {
      git_buf value = GIT_BUF_INIT;
      ~Buffer() { git_buf_dispose(&value); }
    } signature, data;
    const auto code = git_commit_extract_signature(&signature.value, &data.value, repo, &oid, "gpgsig");
    if (code == GIT_ENOTFOUND) {
      return {};
    }
    Check(code, "Read embedded commit signature");
    return CommitSignature{
        std::string(signature.value.ptr, signature.value.size), std::string(data.value.ptr, data.value.size)};
  }, true);
}

Task<GitResult<std::vector<TreeEntry>>> Repository::ReadTreeAsync(ObjectId id) const {
  return Query(state_, [id = std::move(id)](git_repository* repo, std::stop_token) {
    const auto oid = Parse(id);
    auto tree = Make<git_tree, git_tree_free>("Read tree", git_tree_lookup, repo, &oid);
    std::vector<TreeEntry> entries;
    for (std::size_t i = 0; i < git_tree_entrycount(tree.get()); ++i) {
      const auto* entry = git_tree_entry_byindex(tree.get(), i);
      entries.push_back({git_tree_entry_name(entry), Id(git_tree_entry_id(entry)),
                         git_tree_entry_filemode(entry)});
    }
    return entries;
  }, true);
}

Task<GitResult<Bytes>> Repository::ReadBlobAsync(ObjectId id, std::size_t max_bytes) const {
  return Query(state_, [id = std::move(id), max_bytes](git_repository* repo, std::stop_token) {
    const auto oid = Parse(id);
    auto blob = Make<git_blob, git_blob_free>("Read blob", git_blob_lookup, repo, &oid);
    const auto size = git_blob_rawsize(blob.get());
    if (size > max_bytes) {
      Fail(ErrorCode::Unsupported, "Blob exceeds the requested byte limit");
    }
    const auto* bytes = static_cast<const std::byte*>(git_blob_rawcontent(blob.get()));
    return size ? Bytes(bytes, bytes + size) : Bytes{};
  }, true);
}

Task<GitResult<DiffSummary>> Repository::SummarizeDiffAsync(DiffQuery query) const {
  const bool immutable = query.kind == DiffKind::Commit;
  return Query(state_, [query = std::move(query)](git_repository* repo, std::stop_token) {
    std::shared_ptr<const VersionState> version;
    if (query.kind == DiffKind::Staged) {
      version = ReadVersion(repo);
    }
    auto diff = MakeDiff(repo, query);
    DiffSummary result;
    for (std::size_t i = 0; i < git_diff_num_deltas(diff.get()); ++i) {
      result.files.push_back(ReadDelta(*git_diff_get_delta(diff.get(), i)));
    }
    if (version) {
      if (!MatchesVersion(repo, *version)) {
        Fail(ErrorCode::StaleSnapshot, "HEAD or index changed while preparing the reviewed diff");
      }
      result.commit = CommitPrecondition(std::move(version));
    }
    return result;
  }, immutable);
}

Task<GitResult<std::vector<BlameHunk>>> Repository::BlameAsync(std::string path, std::size_t first,
                                                             std::size_t last) const {
  return Query(state_, [path = std::move(path), first, last](git_repository* repo, std::stop_token) {
    ValidatePath(path);
    if (last != 0 && first > last) {
      Fail(ErrorCode::InvalidArgument, "First blame line must not exceed the last blame line");
    }
    git_blame_options options = GIT_BLAME_OPTIONS_INIT;
    options.min_line = first;
    options.max_line = last;
    auto blame = Make<git_blame, git_blame_free>("Blame file", git_blame_file, repo, path.c_str(), &options);
    std::vector<BlameHunk> result;
    for (std::uint32_t i = 0; i < git_blame_get_hunk_count(blame.get()); ++i) {
      const auto* hunk = git_blame_get_hunk_byindex(blame.get(), i);
      result.push_back({hunk->final_start_line_number, hunk->lines_in_hunk, Id(&hunk->final_commit_id),
                        hunk->orig_path ? hunk->orig_path : "", ReadSignature(hunk->final_signature)});
    }
    return result;
  });
}

} // namespace huxerui::git
