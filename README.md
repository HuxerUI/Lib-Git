# Lib-Git

Lib-Git is a coroutine-friendly Git library for [HuxerUI](https://github.com/HuxerUI/HuxerUI). It uses libgit2 to open repositories, inspect history and working changes, update the index, and synchronize with remotes. Applications own review, confirmation, and credential policy.

Link **`HuxerUI::Git`**, include **`<huxerui/git.h>`**, and use the **`huxerui::git`** namespace. [Public API](include/huxerui/git.h)

## Capabilities

| Area | Public API |
| --- | --- |
| Repository access | Discover, open, initialize, and clone. Retain access granted by an SDK `FileReference`. |
| History and inspection | Page commit history, inspect trees and blobs, compare ancestry, read refs, and run blame. |
| Index and commits | Inspect status and diffs, stage or unstage files, hunks, and lines, restore paths, commit, and amend. |
| Integration | Switch branches, reset, merge, rebase, cherry-pick, revert, and resolve conflicts. |
| Repository management | Manage branches, tags, remotes, stashes, submodules, and linked worktrees. |
| Remote transfers | Fetch, pull, and push with typed progress, cooperative stop, and optional credential reuse. |

Interactive linear rebase and Git LFS transfers use an explicitly selected Git executable on supported desktops. A commit can be signed by an application-supplied signer. See the [technical design](docs/lib-git-technical-design.md) for operation contracts and platform requirements.

## Integration

Use HuxerUI SDK 0.5.5 with its matching toolchain and C++20. After creating your application target, acquire Lib-Git with either form below.

### Pin by tag

Set `LIB_GIT_TAG` to a plain Git tag name from the Lib-Git repository.

```cmake
huxerui_use_library(your_app
    TARGET HuxerUI::Git
    URL "https://github.com/HuxerUI/Lib-Git.git"
    TAG "${LIB_GIT_TAG}"
)
```

### Pin by commit

Set `LIB_GIT_COMMIT` to the full 40-character SHA of the revision you want to build.

```cmake
huxerui_use_library(your_app
    TARGET HuxerUI::Git
    URL "https://github.com/HuxerUI/Lib-Git.git"
    COMMIT "${LIB_GIT_COMMIT}"
)
```

### Register the application service

Register `huxerui::git::Install` to resolve `GitService` using `UseService`. Applications can also construct the service directly. `App` is your root view function.

```cpp
#include <huxerui/git.h>
#include <huxerui/huxerui.h>

huxerui::View App();

const huxerui::Application application{
    App,
    {.application_hooks = {huxerui::git::Install}},
};
```

## Open a repository

Run Git tasks through a live HuxerUI `TaskScope`. Blocking Git work runs on library workers and the awaiting task resumes on its application thread.

```cpp
namespace git = huxerui::git;

huxerui::Task<git::GitResult<git::StatusSnapshot>> ReadStatus(
    git::GitService service, huxerui::File directory) {
  auto opened = co_await service.OpenAsync(std::move(directory));
  if (!opened.Succeeded()) {
    co_return git::GitResult<git::StatusSnapshot>::Failure(opened.Error());
  }
  co_return co_await opened.Value().StatusAsync();
}
```

`DiscoverAsync()` finds a repository above a starting directory. Repository handles retain any directory grant used to open them. URI-only providers require an application-accessible local repository before Git can work with it.

Queries return `GitResult<T>`. Writes return typed outcomes with a `WriteReport` and the observed effects. Use the staging snapshot from `ReadPatchAsync()` for hunk or line edits. Commit and amend use a reviewed HEAD and index token. Stale review tokens fail before publication.

## Remote transfers and credentials

`StartClone()`, `StartFetch()`, `StartPull()`, and `StartPush()` admit a job and return a `Transfer` handle. Use `Observe()` to deliver progress to an application scope and `WaitAsync()` for the outcome. `RequestStop()` requests a cooperative stop. Inspect the write effects before retrying an interrupted operation.

Supply a synchronous `CredentialProvider` when constructing `GitService` to resolve credentials automatically. The request carries the operation, canonical local Git directory and remote name when available, and the actual endpoint's authentication challenge. The provider runs on a worker and may be called concurrently.

```cpp
std::optional<git::Credential> FindAccount(const git::CredentialRequest& request);

void InstallGit(huxerui::ApplicationContext& context) {
  context.Provide(std::make_shared<git::GitService>(FindAccount));
}
```

The application implements `FindAccount` using its saved accounts. Lib-Git attempts one automatic lookup per endpoint during a transfer. Return `std::nullopt` when there is no match. A missing or rejected credential allows manual entry through `Transfer::AnswerAuth()` while attempts remain.

The application chooses the pull integration mode, explicit push update policy, and authorization for worktree overwrites. Pull defaults to fast-forward only and push defaults to normal ref updates.

## Platform support

| Feature | macOS | Windows | Linux | iOS | Android | Web |
| --- | --- | --- | --- | --- | --- | --- |
| Discover, open, initialize, and clone local repositories | Yes | Yes | Yes | Yes | Yes | No |
| Read history, refs, status, diffs, and blame | Yes | Yes | Yes | Yes | Yes | No |
| Stage and unstage files, hunks, and lines | Yes | Yes | Yes | Yes | Yes | No |
| Commit, amend, restore, switch branches, and reset | Yes | Yes | Yes | Yes | Yes | No |
| Merge, linear rebase, cherry-pick, revert, and resolve conflicts | Yes | Yes | Yes | Yes | Yes | No |
| Manage branches, tags, remotes, stashes, submodules, and worktrees | Yes | Yes | Yes | Yes | Yes | No |
| Sign commits with an application-supplied signer | Yes | Yes | Yes | Yes | Yes | No |
| Clone, fetch, pull, and push over HTTPS | With TLS backend | With TLS backend | With TLS backend | With TLS backend | With TLS backend | No |
| Clone, fetch, pull, and push over SSH | With SSH backend | With SSH backend | With SSH backend | With SSH backend | With SSH backend | No |
| Shallow clone and fetch | Transport-dependent | Transport-dependent | Transport-dependent | Transport-dependent | Transport-dependent | No |
| Interactive linear rebase | With Git | No | With Git | No | No | No |
| Git LFS transfers | With Git and git-lfs | No | With Git and git-lfs | No | No | No |
| Saved HTTPS credentials in Preview | Keychain | No | No | Keychain | No | No |

Repository access requires an application-accessible local path, including when opening through a `FileReference`. URI-only Android document providers require a local copy.

Use `GitService::GetCapabilities()` to check available library features at runtime. SSH is disabled by default for bundled libgit2. The application-supplied credential provider works with supported transports on each desktop and mobile platform.

## Preview and tests

The [Preview application](examples/preview) demonstrates repository tabs, a virtualized history graph, unified and side-by-side diffs, selective staging, conflict resolution, workspace restoration, and remote synchronization. Use **Repository settings** to bind a saved account to a selected HTTPS remote on Apple platforms. Separate fetch and push URLs have separate bindings. **Remember in Keychain after successful sync** saves a manually entered credential after a successful transfer.

The [test fixtures](tests) use temporary repositories and loopback remotes. They cover stale reviews, interrupted push recovery, distinct accounts at one HTTPS endpoint, manual authentication fallback, and large-history scrolling. The HTTPS fixture uses a temporary CA and runs with a TLS backend that accepts an explicit certificate file, such as OpenSSL.

- [Architecture and operation contracts](docs/lib-git-technical-design.md)
- [Preview interaction design](docs/git-preview-interaction-design.md)

## License

Lib-Git uses the [MIT License](LICENSE). The bundled libgit2 dependency uses GPLv2 with a linking exception. [libgit2 license](https://github.com/libgit2/libgit2/blob/v1.9.7/COPYING)
