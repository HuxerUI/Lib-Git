# Git Preview Desktop Interaction Design

This proposal covers the next iteration of the Lib-Git reference client. It preserves the existing history, diff, staging, and conflict views and defines the desktop interactions around them. The first delivery focuses on reopening a workspace, finding operations where users expect them, and using the main review workflow from the keyboard. The [Lib-Git technical design](lib-git-technical-design.md) defines repository and transport semantics.

The desktop mockup uses sample repository data. [Interactive design](design/git-preview.html) · [Repository chooser](design/git-welcome.png) · [History](design/git-history.png) · [Changes](design/git-changes.png) · [Branch menu](design/git-branch.png) · [Conflicts](design/git-conflicts.png)

## Workspace layout

```text
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│ Lib-Git     [platform ×]  [tools ×]  +                                  Open repository    │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│ platform / main → origin/main      ↑2 ↓1        Refresh   Fetch   Pull…   Push…            │
├────────────────┬───────────────────────────────────────────────────────────────────────────┤
│ History        │ History                                All refs      Go to revision       │
│ Changes     3  │ Graph  Commit                                    Author      Date  Hash   │
│                │ ●─╮    Merge audit context into main             Evan        Nov 25       │
│ LOCAL BRANCHES │ │ ●    Add audit context                         Aria        Nov 24       │
│ main      HEAD │ ●─╯    Set service defaults                      Evan        Nov 22       │
│ feature/a   ⋯  ├───────────────────────────── drag to resize ──────────────────────────────┤
│                │ Commit details / files       │ docs/audit.md            Unified   Split   │
│ REMOTE BRANCHES│ Merge audit context          │ @@ -0,0 +1,3 @@                            │
│ origin/main    │ Compare with parent 1 ▾      │ + # Audit context                          │
│ TAGS           │ A  docs/audit.md              │ + Record the source of each request       │
│ STASHES        │                              │                                            │
├────────────────┴───────────────────────────────────────────────────────────────────────────┤
│ 3 changed paths                                 Last fetched 10:24 · Open sync details     │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

The tab bar identifies the repository. The toolbar identifies the checked-out branch and its configured upstream. The sidebar's primary branch action filters History. Checkout requires the explicit **Switch to branch** command. Show the `HEAD` marker and selection highlight independently so the two states remain distinguishable. Use a path tooltip and a secondary location line when repository names collide. Show ahead and behind counts only after `CompareReachabilityAsync` has produced a result, and label counts limited by shallow history. Counts describe the last known tracking ref until the next fetch.

On History, keep the graph, reference labels, and metadata in one virtualized table. Selecting a row opens its commit details, file list, and first eligible text diff in the bottom pane. Keep parent selection visible on merge commits. On Changes, use the left pane for unstaged and staged file lists and the commit editor. Give the right pane to the selected file. Keep the selected file highlighted in its own group when it has both staged and unstaged edits. Show the two versions independently. Keep the displayed hunk actions, line gutters, and unified or parallel view controls in the diff pane.

```text
Changes                         src/session.cpp · Unstaged             Unified   Split
Unstaged changes (2)            [Stage hunk]  @@ -42,5 +42,6 @@
  M  src/session.cpp              42  42   auto session = OpenSession();
  ?  notes.md                         43 + return RestoreSelection(session);
Staged changes (1)              [Stage hunk]  @@ -93,4 +94,5 @@
  M  src/session.cpp              94  95   SaveSession(session);
──────────────────────────      ──────────────────────────────────────────────────────
Commit message                  2 selected lines                       Stage selected
[Review staged]     [Commit]
```

Show **Conflicts** in the navigation when `ReadOperationAsync()` reports an active operation. Show the operation type and a progress count where the snapshot supports it. Place the conflict list and Continue, Skip, or Abort controls next to the selected conflict. Gate each action on the current snapshot. Keep the existing editor and side choices for this phase. Conflicts can be reopened from the banner even after the user navigates elsewhere.

Use the existing light palette and typography. Give the selected row, keyboard focus, and checked-out branch distinct visual treatments. Keep text labels or symbols alongside added, removed, conflict, and transfer colors. The initial sidebar can grow from the existing 196 logical pixels to about 224 if needed for the branch action button. Retain the desktop split layouts and use a bounded ratio or width per layout instead of persisting absolute coordinates. At compact widths, use the existing drill-down navigation and an explicit **More actions** button. New mobile interactions and provider support need separate device validation.

## Open and resume a workspace

When no repository tab is open, show a compact workspace chooser. Put **Open repository…** and **Clone…** at the top. Below, show up to 15 recent repositories with name and location, newest first. Use the same chooser when adding a tab. Keep path entry and **Initialize…** available in the chooser. Remove a repository from recents through its overflow menu without modifying its files. Recent entries whose location or access changed stay visible with **Locate…** or **Grant access…** as appropriate.

```text
┌─ Open repository ──────────────────────────────────────────────────┐
│ [Choose folder…]  [Clone…]                         Enter path…     │
│                                                                    │
│ Recent repositories                                                │
│ platform           ~/Projects/team/platform                Open    │
│ tools              ~/Projects/team/tools                   Open    │
│ archive            External volume unavailable             Locate… │
│                                                                    │
│ Initialize a repository…                                           │
└────────────────────────────────────────────────────────────────────┘
```

At launch, render the shell before repository I/O. Restore tab placeholders in order and prioritize opening the last active tab. Start the remaining opens with bounded concurrency and defer history, status, and diff loads for inactive tabs until selection. Close a placeholder locally, even while its open task is pending. On completion, apply results only to the tab ID and restore generation that initiated the work. Opening from the chooser activates or replaces the matching placeholder instead of creating a duplicate.

For an accessible tab, open the repository and inspect fresh Git state. Check for an active operation before restoring the view. Resume Conflicts when an operation is present, even with zero unresolved paths, so the user can continue if Git allows it. Otherwise, restore a valid history filter and then the selected commit or file. Fall back to all refs for a deleted branch, to the first visible commit for an unreachable selection, or to the file list for a path no longer in the chosen comparison. Treat a bare repository as History-only. Treat a moved folder or revoked grant as a recoverable tab state. Never browse a saved directory by constructing a `File` from a grant's display path. View restoration does not issue Git writes or remote transfers.

### Session data and ownership

Keep one app-owned session model. Persist the ordered list of open tab IDs, the active tab ID, recent locations, and desktop split preferences under `ApplicationHandle::Directories().data_directory`. Each tab records a location ID, page, full branch ref filter, commit OID and parent, repository-relative selected path and comparison kind, and diff mode. Keep the selected commit distinct from a viewport anchor. Restore an exact graph anchor if its OID is already in a loaded page, and otherwise scroll to the loaded selection or top. Restoration does not walk the full history just to recover a scroll position. A fresh worktree or staged patch starts with no selected lines. Keep drafts in memory in this phase and confirm closing a tab or the last window when it would discard a nonempty commit draft. Conflict buffer recovery is a separate editor decision. Only resolutions already written to Git survive restart.

Represent an app-accessible path as a local location. Represent a picker grant using an app-defined key and a display hint. Persist the key after `FileReference::RememberAsync()` succeeds. Use `FileReference::RestoreAsync()` to reacquire the grant and pass the reference to `GitService::OpenAsync()`. If a grant cannot be remembered, allow use for the active session and show that reopening will require permission. When reauthorizing or relocating, verify the new selection opens before replacing the saved entry. Keep the old grant record until the updated session is stored. Forget a grant after its location is removed and neither an open tab nor a recent uses it. The Git resolver and current filesystem permissions still decide whether a restored reference can provide a usable repository.

Use a stable app location ID for the manifest and the Git-resolved `git_directory` for deduplication after open. Opening the same worktree through its root or `.git` should activate the existing tab. Linked worktrees have separate Git directories and keep separate tabs, views, and selections. Verify aliases and case behavior on each desktop filesystem. If two restored placements resolve to the same worktree, keep one and map the saved active ID to it.

Store one small versioned snapshot. Coalesce changes on the app thread and serialize file writes through one application-level writer. Write a complete snapshot to a sibling temporary file, then replace the saved file using `File::MoveToAsync()` where same-directory replacement is validated. If writing or replacement fails, keep the last valid saved state and retry a newer snapshot later. If parsing fails or the schema version is unsupported, show the chooser and preserve the original before automatic saving can resume. Validate replace behavior and abrupt termination on the target desktop filesystems. The public `File` contract does not promise power-loss durability or a cross-process transaction. This phase covers one desktop workspace per running app instance. It leaves settings synchronization between independent processes for a separate design.

Persist UI intent and locations only. `FileReview`, patch snapshots, commit preconditions, operation versions, cursors, transfer handles, credentials, and diff bytes are live state. Reload those from Lib-Git when needed. After application restart, an interrupted transfer has no resumable handle. A user who needs to verify a previous push can inspect fresh remote refs through the sync flow. Normal push remains non-forcing.

## Branch interactions

Clicking a branch row filters History. A secondary click opens the clicked row's menu without changing the history filter or current branch. Add a trailing **More actions** button for discovery and touch. On keyboard focus, the Context Menu key or Shift+F10 opens the same actions. Each action names its target and carries the originating tab ID, full ref, and observed tip. If the tab is gone or has been replaced, dismiss the command and return focus to the workspace. Different workspace tabs never share command targets.

| Action | Local branch | Remote-tracking branch | Execution and safeguards |
| --- | --- | --- | --- |
| View history and copy ref name | Yes | Yes | Filter to this ref or copy its full name. Allow both while a write is blocked when the information is available. |
| Switch to branch | If it is neither current nor checked out in another worktree | Through the tracking-branch creation flow | Call `SwitchAsync` with the full local ref, fresh observed HEAD, and safe checkout. The local branch's current tip is the destination. If checkout is blocked, show the affected paths when present. The menu does not offer a force retry. |
| Create local branch here | Yes | Yes | Show the starting OID and a name field. `CreateBranchAsync` is non-overwriting. Offer an explicit switch and, for a remote-tracking target, an explicit upstream. Report branch creation, tracking setup, and switch as separate results if a later step fails. |
| Rename or change upstream | Yes, subject to checkout occupancy |  | For Rename, pass the observed tip to `RenameBranchAsync`. For tracking, offer valid remote-tracking refs and a clear action. Show the existing and new value and refresh after `SetUpstreamAsync`. The existing API treats the local branch ref name as its target. |
| Delete local branch | If it is neither current nor checked out in any worktree |  | Confirm full name and tip before calling `DeleteBranchAsync`. Use reachability relative to the displayed current branch for context. If history is shallow, the check fails, or the branch has unique commits, show that explicitly. Require confirmation in every case. Remote branch deletion is outside this menu. |
| Merge into current branch or rebase current branch onto this tip | If a different eligible tip is selected | If the tracking tip is eligible | Show source and destination names and OIDs. Use the selected OID as the input. Rebase here means the existing linear operation. Require the guarded integration behavior described below before exposing these actions in the redesigned menu. |

Disallow worktree-changing actions in bare repositories, during a conflicting operation, or while an incompatible write is pending. Explain blocking conditions in nearby status or in the dialog. Preserve the conflict workflow if a valid integration enters it. When a ref disappears or changes after a reviewed rename, delete, or integration dialog, refresh and let the user review the updated target. The preview rechecks the displayed source ref and passes the reviewed input OID and destination HEAD to the guarded `MergeAsync` or `RebaseAsync` overload. The library checks the destination inside its serialized write and immediately before entering libgit2's integration. Merge publication also checks HEAD. A separate process can still change the checkout during a libgit2 call, so the design does not promise a transaction covering all external interleavings. [Public API](../include/huxerui/git.h) · [Integration writes](../src/integration.cpp)

## Keyboard and focus

Use the platform's primary modifier (`Command` on macOS, `Control` on Windows and Linux). Bind shortcuts once in the window through `WindowHandle::OnShortcut()` and dispatch through the same app actions as buttons and menus. A matching shortcut runs only if its command is available and belongs to the active context. When a dialog or menu owns focus, contain input within that layer. Let the focused editor keep its character input, selection, editing, and IME commands. Escape closes the topmost menu or dialog and returns focus to its source. Escape does not abort Git work.

| Shortcut | Result |
| --- | --- |
| Primary+O | Show the open repository chooser with the main open action focused. |
| Primary+T | Add a repository tab through the same chooser. |
| Primary+W | Close the current repository tab or dismiss the add flow. When the last tab closes, show the chooser. Follow the normal window close path if there is no tab or add flow to close. |
| Primary+1 / Primary+2 / Primary+3 | Focus History, Changes, or Conflicts when the page is available. |
| Primary+L | Focus Go to revision in History. Enter resolves the typed revision and opens its commit review. |
| Primary+R | Refresh the active repository with the same policy as the toolbar. |
| Shift+F10 / Context Menu key | Open the focused row's actions where the host delivers the shortcut. |
| Tab / Shift+Tab | Traverse reachable controls with a visible focus indicator. List navigation uses Up and Down within the focused list, with Enter to activate the selected row. |

Arrow navigation applies within the active list. Move or scroll the virtualized list before requesting focus on a row outside its current viewport. In History, moving the selection updates the read-only review. In Changes, activating a file or line selection does not stage it. Enter or Space on a focused **Stage hunk** button invokes that button. The first shortcut set has no direct binding for commit, push, branch deletion, or abort. Use focusable controls and semantic labels for branch menus, repository status, and every staging action. Verify focus after closing overlays, switching tabs, and recycling virtual rows.

## Sync and conflict presentation

Keep Fetch directly in the toolbar. Pull and Push open a small confirmation surface naming the local branch, remote, destination ref, and operation mode. The initial Pull default remains fast-forward only. If history has diverged, show what was fetched and offer the separate merge or rebase flow with a new review. Push uses the reviewed source OID and the non-forcing refspec. Show transfer progress and authentication in the application-owned transfer area so changing tabs cannot misattribute the operation. For an interrupted push with an unknown remote result, offer **Check remote refs** before offering a retry. Keep terminal success, rejection, and partial results visible with the repository and remote names. Live transfer persistence and new transport features are outside this design.

## HuxerUI fit and verification

| Status | Finding |
| --- | --- |
| Verified in the current Preview | The app already has tab state, separate display pages, `SplitLayout`, review generations, virtualized history and diff, and operation snapshots. The repository test opens a second repository, verifies hunk staging against Git, and completes a merge conflict. macOS screenshots and the existing large-history fixture establish the desktop baseline. [Preview source](../examples/preview/src/app.cpp) · [Repository tests](../tests/repository.cpp) · [Large history fixture](../tests/large_history.py) |
| Verified in SDK 0.5.5 headers | `ViewEvents::ContextMenuRequested`, `UseMenu()` and `MenuHandle::ShowAt()` cover secondary click and keyboard context requests. `WindowHandle::OnShortcut()` is lifecycle-bound and runs after focused controls. `UseViewHandle()` provides focus requests. File and FileReference provide app storage and supported grant restoration. See `event.h`, `presentation.h`, `window.h`, `modifier.h`, `app.h`, and `file.h` in the installed HuxerUI SDK. |
| Verified in the current library | Branch operations expose tip and checkout guards as described above. `DeleteBranchAsync()` checks occupancy and expected tip and does not check mergedness. Integration accepts an input OID. The existing Git API distinguishes operation snapshots, worktree generations, and transfer outcomes. [Public header](../include/huxerui/git.h) · [Branch writes](../src/repository_management.cpp) · [Integration](../src/integration.cpp) |
| Proposed for prototype | Verify focus restoration for virtualized rows and overlays, secondary and keyboard menu dispatch, per-platform shortcut conflicts, and directory grant recovery. Verify sibling-file replacement and abrupt termination behavior. Profile active-first restoration with several large repositories. Validate an expected-HEAD integration path under concurrent external writes separately. |

Use existing HuxerUI menus, dialogs, tasks, and focus handling. Keep a small set of app actions shared by UI entry points and a dedicated session serializer. A plugin command system, global event bus, general settings database, and new Git repository backend would add work with no requirement here. Keep section splitting within the example, where it can follow the existing repository, history, changes, and conflict flows.

## Delivery and acceptance

| Delivery slice | Exit criteria |
| --- | --- |
| Workspace chooser and restoration | Restart with three tabs including two linked worktrees and restore order, the active tab, and eligible review selections. Open the same worktree via an alias and get one tab. With a missing path, revoked grant, malformed snapshot, or interrupted write, retain access to the other tabs and show a recovery action. Ensure initial rendering and active repository use are independent of inactive repository scans. A restart with an in-progress merge or rebase opens the actual Git operation. |
| Menus and keyboard | Right-click an unselected local or remote-tracking row and verify that both the current branch and history selection remain unchanged. Open the same menu from the keyboard and the More button. Test active, occupied, deleted, and moved-tip branches against Git. Verify shortcuts from the history list, the commit editor, and a modal. Return focus after dismissal and after changing tabs. For guarded integration, race a confirmed action with an external HEAD update and verify the operation does not run against the wrong branch. |
| Sync and platform review | In the packaged macOS app, open a local repository, filter history, review two hunks, and verify the actual index after staging one. Confirm the source and destination in sync dialogs and use a local bare remote or loopback transport to test an interrupted push with an unknown outcome. Run menu, focus, and persistence smoke tests on packaged Windows and Linux builds before claiming parity. Verify that compact layout retains existing navigation and that unsupported directory providers receive a recovery action. |
