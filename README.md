# Lib-Git

Lib-Git provides a C++ Git client API for HuxerUI. It uses libgit2 for repository inspection, guarded index updates, commit and amend, history integration, and remote transfers. The preview has repository tabs, a synchronization toolbar, and dedicated History, Changes, and Conflicts workspaces. Desktop History spans the workspace above a resizable commit review area. Changes and Conflicts place the file list next to a resizable diff or conflict editor. Narrow layouts open commit, diff, and conflict details as separate pages.

The [technical design](docs/lib-git-technical-design.md) describes the target product, concurrency contracts, and release gates.

## Build and run

Use HuxerUI SDK 0.5.5 with its matching toolchain. The CMake path downloads the pinned libgit2 1.9.7 source archive, verifies its SHA-256 digest, and links it statically. An existing source directory can be selected with `FETCHCONTENT_SOURCE_DIR_LIBGIT2`.

```sh
cmake -S examples/preview -B build/preview -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/preview
```

On macOS, open `build/preview/example_git.app`. From the opening screen, choose a local repository, clone into a new directory, or initialize one. App storage fills in a path under the application's durable data directory. In History, select a branch or enter a revision to inspect a commit and its changed files. Unified and side-by-side diffs show all hunks in one scrollable view, with horizontal scrolling for long lines. In Changes, stage files, hunks, or lines and use Review staged before committing. Identity sets the author name and email for that repository.

The preview restores recent repositories and open tabs from its application data directory. A moved repository or expired grant can be located again from the affected tab. Select a branch to filter history, or open its action menu with a secondary click, the More button, or the keyboard context-menu command. Pull and Push show the reviewed endpoints before starting. Command is the primary shortcut modifier on macOS and Control is the primary modifier on Windows and Linux. Use Primary+O to open the chooser, Primary+1 through Primary+3 to change views, and Primary+L to focus the revision field. [Desktop interaction design](docs/git-preview-interaction-design.md)

Build the iOS simulator and Android debug shells from `examples/preview`. For iOS, use the device selector reported by `huxerui devices`.

```sh
huxerui build ios --build-system cmake --device "$SIMULATOR_ID" --config debug
huxerui build android --build-system cmake --abi arm64-v8a --config debug
```

Enable SSH with `-DHUXERUI_GIT_SSH=libssh2` and a compatible libssh2 installation, or select the libgit2 OpenSSH transport on a supported desktop. HTTPS uses the selected libgit2 TLS provider. The default Android CMake build links Mbed TLS 3.6.7 and loads Android's system CA directory. [Mbed TLS release and checksum](https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7)

For mcpp, the manifest resolves the sibling HuxerUI source checkout. Point `HUXERUI_GIT_LIBGIT2_PREFIX` at libgit2 1.9.7 installed for the selected mcpp target, including its static archive, public headers, and pkg-config metadata. The HuxerUI rules generate the module from the public headers.

```sh
HUXERUI_GIT_LIBGIT2_PREFIX=/path/to/target/libgit2 mcpp build
```

## Validation and support

```sh
cmake -S . -B build/library -G Ninja -DHUXERUI_GIT_BUILD_TESTS=ON
cmake --build build/library
ctest --test-dir build/library --output-on-failure
python3 tests/large_history.py build/library/huxerui_git_repository_test "$(command -v git)"
```

The tests use temporary repositories, the installed Git executable, and the HuxerUI windowless fixture. The loopback Smart HTTP test drops a receive-pack response after the remote applies the update, then checks the actual remote ref. Tests cover selective index updates, stale reviews, guarded integration after an external HEAD change, signed-object round trips, local push and clone, branch menus, conflict resolution, and workspace restoration with a linked worktree and a missing path. If `git-lfs` is available, the repository test also exercises an upload to a local bare remote. The large-history fixture opens 100,000 linear commits and scrolls the graph.

- macOS arm64 is the tested desktop build. A separate mcpp consumer imported and linked `huxerui.git` on the same target. The iOS arm64 simulator and Android arm64 debug apps have been built, installed, and launched. Mobile remote transport and prolonged filesystem access require device validation. Windows and Linux have CMake entry points and need runtime and packaging tests. The Web capability reports local repository processing unavailable.
- The preview refreshes status and reference changes when its window becomes active, and Refresh reloads the current local review. A filesystem watcher for live updates while the window stays active remains a release gate.
- Interactive linear rebase and Git LFS transfer commands have an explicit Git executable backend on macOS and Linux. Commands use the installed executable and require trusted repository configuration. Git LFS requires the installed extension. Full LFS file-filter and locking workflows, merge-preserving interactive rebase, and Windows CLI parity remain release gates.
- The public signing callback accepts exact commit bytes and verifies the review again before publication. The application supplies the signer and any trust verification. A repository requiring unsupported hooks or filters receives an explicit error from the libgit2 workflow.

Lib-Git is MIT licensed. Its bundled libgit2 dependency uses the GPLv2 with a linking exception. [libgit2 license and exception](https://github.com/libgit2/libgit2/blob/v1.9.7/COPYING)
