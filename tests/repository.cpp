#include <huxerui/git.h>
#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

#include "../examples/preview/src/workspace_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace huxerui;
namespace git = huxerui::git;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

View App();

View PreviewWithSession(File file);

namespace {

std::optional<File> preview_session;

/// Loads the installed SDK assets used by menu and dialog controls in the windowless fixture.
class PreviewResources final : public PlatformResources {
public:
  ResourceConfiguration Configuration() const override { return {}; }

  std::optional<InputStream> OpenRead(std::string_view package_path) override {
    auto result = File(std::string(GIT_TEST_RESOURCE_PACKAGE) + "/" + std::string(package_path)).OpenRead();
    if (!result.Succeeded()) return {};
    return std::move(result).Value();
  }
};

View PreviewRoot() {
  return preview_session ? PreviewWithSession(*preview_session) : App();
}

void Require(bool condition, std::string message) {
  if (!condition) {
    throw std::runtime_error(std::move(message));
  }
}

template <class T> T Value(git::GitResult<T> result) {
  if (!result.Succeeded()) {
    throw std::runtime_error(result.Error().message);
  }
  return std::move(result).Value();
}

void Succeeded(const git::WriteReport& report) {
  Require(report.Succeeded(), report.issue ? report.issue->message : "Expected a successful Git write");
}

std::string Quoted(std::string_view text) {
#ifdef _WIN32
  Require(text.find_first_of("\"%") == std::string_view::npos, "Unsupported test fixture path");
  return "\"" + std::string(text) + "\"";
#else
  std::string result = "'";
  for (const char value : text) {
    if (value == '\'') result += "'\\''";
    else result += value;
  }
  return result + "'";
#endif
}

void RunGit(const fs::path& directory, std::string_view command) {
  const auto invocation = Quoted(GIT_TEST_EXECUTABLE) + " -C " + Quoted(directory.string()) + " " +
                          std::string(command);
  Require(std::system(invocation.c_str()) == 0, "Fixture Git command failed: " + std::string(command));
}

void Write(const fs::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  Require(out.good(), "Write fixture file");
}

std::string Read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  Require(input.good(), "Read fixture file");
  return std::string(std::istreambuf_iterator<char>(input), {});
}

struct Fixture {
  fs::path root;
  fs::path repository;
  fs::path remote;
  fs::path output;
  std::optional<std::string> smart_remote;
  bool done = false;
  std::string failure;

  Fixture() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    root = fs::temp_directory_path() / ("huxerui-git-" + std::to_string(tick));
    fs::create_directory(root);
    repository = root / "worktree";
    remote = root / "remote";
    output = root / "output";
    fs::create_directory(repository);
    fs::create_directory(remote);
    RunGit(repository, "init --initial-branch=main");
    RunGit(repository, "config user.name " + Quoted("Lib Git Tests"));
    RunGit(repository, "config user.email lib-git@example.test");
    RunGit(repository, "config commit.gpgsign false");
    RunGit(repository, "config core.autocrlf false");
    RunGit(remote, "init --bare --initial-branch=main");
    Write(repository / "notes.txt", Text());
    Write(repository / "other.txt", "other\n");
    RunGit(repository, "add notes.txt other.txt");
    RunGit(repository, "commit -m baseline");
    RunGit(repository, "remote add origin " + Quoted(remote.string()));
  }

  ~Fixture() {
    std::error_code error;
    fs::remove_all(root, error);
  }

  static std::string Text(std::string_view top = "line 2", std::string_view bottom = "line 25") {
    std::string text;
    for (int i = 1; i <= 30; ++i) {
      if (i == 2) text += top;
      else if (i == 25) text += bottom;
      else text += "line " + std::to_string(i);
      text += '\n';
    }
    return text;
  }

  std::string InspectGit(std::string command) {
    RunGit(repository, command + " > " + Quoted(output.string()));
    return Read(output);
  }
};

Task<void> VerifyLiteralIndexRestore(Fixture& fixture, const git::Repository& repo) {
  const auto literal = fixture.repository / "literal[1].txt";
  Write(literal, "literal edit\n");
  Write(fixture.repository / "literal1.txt", "matching edit\n");
  Write(fixture.repository / "notes.txt", "tracked edit\n");
  RunGit(fixture.repository, "--literal-pathspecs add -- " + Quoted("literal[1].txt") + " literal1.txt notes.txt");

  const auto restored = co_await repo.RestoreAsync(
      {{"literal[1].txt", "notes.txt"}, git::RestoreSource::Head, git::RestoreTarget::Index});
  Succeeded(restored.report);
  Require(restored.index == git::WriteEffect::Changed && restored.worktree == git::WriteEffect::Unchanged,
          "Restoring from HEAD into the index must report only an index change");
  Require(fixture.InspectGit("diff --cached --name-only") == "literal1.txt\n" &&
              fixture.InspectGit("show :literal1.txt") == "matching edit\n" &&
              fixture.InspectGit("show :notes.txt") == Fixture::Text(),
          "Restoring literal paths must preserve the staged entry that matches the filename as a pattern");
  Require(Read(literal) == "literal edit\n" && Read(fixture.repository / "notes.txt") == "tracked edit\n",
          "Restoring the index must leave the worktree contents intact");

  const auto unchanged = co_await repo.RestoreAsync(
      {{"literal[1].txt"}, git::RestoreSource::Head, git::RestoreTarget::Index});
  Require(unchanged.report.status == git::WriteStatus::NoChange &&
              unchanged.index == git::WriteEffect::Unchanged,
          "An unstaged literal path must leave every index entry untouched");

  RunGit(fixture.repository, "reset --hard HEAD");
  fs::remove(literal);
}

Task<void> VerifyBranchRename(Fixture& fixture, const git::Repository& repo) {
  RunGit(fixture.repository, "switch -c review.old");
  RunGit(fixture.repository, "config branch.review.old.remote origin");
  RunGit(fixture.repository, "config branch.review.old.merge refs/heads/main");
  RunGit(fixture.repository, "config --add branch.review.old.custom first");
  RunGit(fixture.repository, "config --add branch.review.old.custom second");
  RunGit(fixture.repository, "config branch.review.old.child.description separate");
  const auto expected = Value(co_await repo.ResolveRevisionAsync("refs/heads/review.old"));
  const auto original_log = fixture.InspectGit("reflog show --format=%gs refs/heads/review.old");
  const auto lock = fixture.repository / ".git/refs/heads/review.new.lock";
  Write(lock, "");

  const auto busy = co_await repo.RenameBranchAsync("review.old", "review.new", expected);
  Require(busy.report.issue && busy.report.issue->code == git::ErrorCode::Busy &&
              busy.references == git::WriteEffect::Unchanged,
          "A locked destination must reject branch rename");
  const auto source = co_await repo.ResolveRevisionAsync("refs/heads/review.old");
  const auto destination = co_await repo.ResolveRevisionAsync("refs/heads/review.new");
  Require(source.Succeeded() && source.Value() == expected && !destination.Succeeded() && fs::exists(lock),
          "A rejected rename must preserve the original branch and the destination lock");
  Require(fixture.InspectGit("symbolic-ref HEAD") == "refs/heads/review.old\n" &&
              fixture.InspectGit("config --get branch.review.old.remote") == "origin\n" &&
              fixture.InspectGit("reflog show --format=%gs refs/heads/review.old") == original_log,
          "A rejected rename must preserve HEAD, the upstream, and the branch reflog");
  fs::remove(lock);

  const auto overlap = co_await repo.RenameBranchAsync("review.old", "review.old/nested", expected);
  Require(overlap.report.issue && overlap.report.issue->code == git::ErrorCode::Unsupported &&
              overlap.references == git::WriteEffect::Unchanged &&
              Value(co_await repo.ResolveRevisionAsync("refs/heads/review.old")) == expected,
          "Overlapping branch names must preserve the original branch");

  const auto renamed = co_await repo.RenameBranchAsync("refs/heads/review.old", "review.new", expected);
  Succeeded(renamed.report);
  const auto removed = co_await repo.ResolveRevisionAsync("refs/heads/review.old");
  const auto renamed_tip = Value(co_await repo.ResolveRevisionAsync("refs/heads/review.new"));
  Require(renamed.references == git::WriteEffect::Changed && !removed.Succeeded() && renamed_tip == expected &&
              fixture.InspectGit("symbolic-ref HEAD") == "refs/heads/review.new\n",
          "A successful rename must move the original tip and the current HEAD");
  Require(fixture.InspectGit("config --get branch.review.new.remote") == "origin\n" &&
              fixture.InspectGit("config --get branch.review.new.merge") == "refs/heads/main\n" &&
              fixture.InspectGit("config --get-all branch.review.new.custom") == "first\nsecond\n" &&
              fixture.InspectGit("config --get-regexp " + Quoted("^branch\\.review\\.old\\.")) ==
                  "branch.review.old.child.description separate\n",
          "A successful rename must migrate the complete branch configuration without changing a dotted sibling");
  Require(fixture.InspectGit("reflog show --format=%gs refs/heads/review.new") ==
              "branch: renamed refs/heads/review.old to refs/heads/review.new\n" + original_log,
          "A successful rename must preserve the branch reflog");

  RunGit(fixture.repository, "switch main");
  RunGit(fixture.repository, "branch -D review.new");
  RunGit(fixture.repository, "config --remove-section branch.review.old.child");
}

Task<void> Verify(Fixture& fixture) {
  try {
    git::GitService service;
    const auto source = File(fixture.repository.string());
    auto repo = Value(co_await service.OpenAsync(source));
    Succeeded((co_await repo.SetIdentityAsync({"Lib Git Tests", "lib-git@example.test"})).report);
    const auto identity = Value(co_await repo.ReadIdentityAsync());
    Require(identity && identity->email == "lib-git@example.test", "Read persisted repository identity");
    auto history = Value(co_await repo.OpenHistoryAsync());
    auto page = Value(co_await history.NextAsync());
    Require(page.commits.size() == 1 && page.commits.front().subject == "baseline", "Expected baseline history");
    co_await VerifyLiteralIndexRestore(fixture, repo);
    co_await VerifyBranchRename(fixture, repo);

    auto invalid_path_task = repo.BlameAsync("../notes.txt");
    const auto invalid_path = co_await std::move(invalid_path_task);
    Require(!invalid_path.Succeeded() && invalid_path.Error().code == git::ErrorCode::InvalidArgument,
            "Blame must return an invalid path through its task result");

    const auto invalid_range = co_await repo.BlameAsync("notes.txt", 4, 2);
    Require(!invalid_range.Succeeded() && invalid_range.Error().code == git::ErrorCode::InvalidArgument,
            "Blame must reject a reversed line range");

    const auto attribution = Value(co_await repo.BlameAsync("notes.txt", 2, 3));
    Require(attribution.size() == 1 && attribution.front().first_line == 2 &&
            attribution.front().line_count == 2 && attribution.front().commit == page.commits.front().id,
            "Blame must attribute the requested inclusive range to the baseline commit");

    Write(fixture.repository / "notes.txt", Fixture::Text("top edit", "bottom edit"));
    auto patch = Value(co_await repo.ReadPatchAsync({{}, "notes.txt"}));
    Require(patch.staging.IsValid() && patch.hunks.size() == 2, "Expected two selectable hunks");
    auto staged = co_await repo.ApplySelectionAsync(
        {git::PatchAction::Stage, patch.staging, {patch.hunks.front().id}, {}});
    Succeeded(staged.report);
    Require(fixture.InspectGit("show :notes.txt") == Fixture::Text("top edit"),
            "Only the selected hunk should reach the index");
    Require(Read(fixture.repository / "notes.txt") == Fixture::Text("top edit", "bottom edit"),
            "Partial staging must leave worktree bytes intact");

    auto staged_patch = Value(co_await repo.ReadPatchAsync({{.kind = git::DiffKind::Staged}, "notes.txt"}));
    auto unstaged = co_await repo.ApplySelectionAsync(
        {git::PatchAction::Unstage, staged_patch.staging, {staged_patch.hunks.front().id}, {}});
    Succeeded(unstaged.report);
    Require(fixture.InspectGit("show :notes.txt") == Fixture::Text(), "The hunk should be unstaged");

    auto old = Value(co_await repo.ReadPatchAsync({{}, "notes.txt"}));
    Write(fixture.repository / "notes.txt", Fixture::Text("new edit", "bottom edit"));
    const auto stale = co_await repo.ApplySelectionAsync(
        {git::PatchAction::Stage, old.staging, {old.hunks.front().id}, {}});
    Require(stale.report.issue && stale.report.issue->code == git::ErrorCode::StaleSnapshot,
            "A changed source file must invalidate the hunk");

    auto fresh = Value(co_await repo.ReadPatchAsync({{}, "notes.txt"}));
    Write(fixture.repository / "other.txt", "added by system Git\n");
    RunGit(fixture.repository, "add other.txt");
    Succeeded((co_await repo.ApplySelectionAsync(
        {git::PatchAction::Stage, fresh.staging, {fresh.hunks.front().id}, {}})).report);
    Require(fixture.InspectGit("show :other.txt") == "added by system Git\n",
            "A separate index entry added externally must remain staged");

    auto review = Value(co_await repo.SummarizeDiffAsync({.kind = git::DiffKind::Staged}));
    Write(fixture.repository / "other.txt", "changed after review\n");
    RunGit(fixture.repository, "add other.txt");
    auto refused = co_await repo.CommitAsync({review.commit, "stale review"});
    Require(refused.report.issue && refused.report.issue->code == git::ErrorCode::StaleSnapshot,
            "Commit must reject a changed index");
    review = Value(co_await repo.SummarizeDiffAsync({.kind = git::DiffKind::Staged}));
    auto commit = co_await repo.CommitAsync({review.commit, "selected changes"});
    Succeeded(commit.report);
    Require(fixture.InspectGit("show HEAD:notes.txt") == Fixture::Text("new edit"),
            "Commit should contain the reviewed index");

    auto sign_review = Value(co_await repo.SummarizeDiffAsync({.kind = git::DiffKind::Staged}));
    std::string signed_bytes;
    git::CommitRequest replacement{.expected = sign_review.commit, .message = "selected changes",
        .signer = [&](std::string_view input) {
          signed_bytes = input;
          return git::GitResult<std::string>::Success("fixture signature");
        }};
    commit = co_await repo.AmendAsync(std::move(replacement));
    Succeeded(commit.report);
    const auto signature = Value(co_await repo.ReadCommitSignatureAsync(*commit.created_commit));
    Require(signature && signature->signature == "fixture signature" && signature->signed_data == signed_bytes,
            "The signature API must preserve the exact signed commit contents");

    auto push = Value(service.StartPush(repo, {"origin", {{commit.created_commit, "refs/heads/main"}}}));
    const auto pushed = co_await push.WaitAsync();
    Succeeded(pushed.report);
    const auto refs = Value(co_await service.ReadRemoteRefsAsync(repo, "origin"));
    Require(std::ranges::any_of(refs, [&](const git::RefInfo& ref) {
      return ref.name == "refs/heads/main" && ref.target == commit.created_commit;
    }), "Push must update the bare remote");

    if (fixture.smart_remote) {
      Succeeded((co_await repo.AddRemoteAsync("smart", *fixture.smart_remote)).report);
      auto interrupted = Value(service.StartPush(repo, {"smart", {{commit.created_commit, "refs/heads/main"}}}));
      const auto unknown = co_await interrupted.WaitAsync();
      Require(unknown.references.size() == 1 && unknown.references.front().status == git::RefPushStatus::Unknown,
              "A lost Smart HTTP push response must be reported as unknown");
      const auto observed = Value(co_await service.ReadRemoteRefsAsync(repo, "smart"));
      Require(std::ranges::any_of(observed, [&](const git::RefInfo& ref) {
        return ref.name == "refs/heads/main" && ref.target == commit.created_commit;
      }), "The remote ref query must recover the actual push outcome");
    }

    auto clone = Value(service.StartClone({fixture.remote.string(), File((fixture.root / "clone").string())}));
    const auto cloned = co_await clone.WaitAsync();
    Succeeded(cloned.report);
    Require(cloned.repository.has_value(), "Clone must return an open repository");
    Require(Read(fixture.root / "clone" / "notes.txt") == Fixture::Text("new edit"), "Clone must check out the branch");
    const auto first_wait = co_await clone.WaitAsync();
    Require(first_wait.report.Succeeded(), "A second wait observes the same result");
    const auto clone_info = Value(co_await cloned.repository->InspectAsync());
    auto pull = Value(service.StartPull(*cloned.repository, {{}, clone_info.head, "refs/remotes/origin/main"}));
    const auto pulled = co_await pull.WaitAsync();
    Succeeded(pulled.report);
    Require(pulled.fetch && pulled.integration && pulled.integration->report.status == git::WriteStatus::NoChange,
            "Pull must resolve an up-to-date upstream from this fetch");

    RunGit(fixture.repository, "reset --hard");
    RunGit(fixture.repository, "branch topic");
    Write(fixture.repository / "notes.txt", Fixture::Text("main content"));
    RunGit(fixture.repository, "commit -am main");
    RunGit(fixture.repository, "switch topic");
    Write(fixture.repository / "notes.txt", Fixture::Text("topic content"));
    RunGit(fixture.repository, "commit -am topic");
    RunGit(fixture.repository, "switch main");
    const auto topic = Value(co_await repo.ResolveRevisionAsync("topic"));
    const auto reviewed_head = Value(co_await repo.InspectAsync()).head;
    RunGit(fixture.repository, "switch topic");
    const auto stale_merge = co_await repo.MergeAsync(topic, reviewed_head);
    const auto stale_rebase = co_await repo.RebaseAsync(topic, reviewed_head);
    Require(stale_merge.report.issue && stale_merge.report.issue->code == git::ErrorCode::StaleSnapshot &&
            stale_rebase.report.issue && stale_rebase.report.issue->code == git::ErrorCode::StaleSnapshot,
            "Guarded integration must reject a branch switched after review");
    Require(fixture.InspectGit("symbolic-ref HEAD").find("refs/heads/topic") != std::string::npos &&
            fixture.InspectGit("status --porcelain").empty(), "Stale integration must leave Git unchanged");
    RunGit(fixture.repository, "switch main");
    auto merge = co_await repo.MergeAsync(topic, reviewed_head);
    Require(merge.report.status == git::WriteStatus::NeedsResolution, "Expected merge conflict");
    auto reopened = Value(co_await service.OpenAsync(source));
    auto operation = Value(co_await reopened.ReadOperationAsync());
    Require(operation && operation->conflicts.size() == 1, "Conflict must survive reopen");
    auto resolution = co_await reopened.ResolveConflictAsync(
        {operation->version, operation->conflicts.front().path, git::Resolution::Theirs});
    Succeeded(resolution.report);
    const auto old_step = co_await reopened.ContinueAsync(operation->version);
    Require(old_step.report.issue && old_step.report.issue->code == git::ErrorCode::StaleSnapshot,
            "Continue must reject the version before resolution");
    operation = Value(co_await reopened.ReadOperationAsync());
    Require(operation && operation->can_continue, "Resolved merge can continue");
    auto continued = co_await reopened.ContinueAsync(operation->version);
    Succeeded(continued.report);
    auto detail = Value(co_await reopened.ReadCommitAsync(*continued.commit));
    Require(detail.summary.parents.size() == 2, "Merge commit must contain two parents");
    Require(fixture.InspectGit("show HEAD:notes.txt") == Fixture::Text("topic content"),
            "Merge should commit the selected side");

    RunGit(fixture.repository, "switch -c replay topic~1");
    Write(fixture.repository / "notes.txt", Fixture::Text("replay change"));
    RunGit(fixture.repository, "commit -am replay");
    const auto replay_head = Value(co_await reopened.ResolveRevisionAsync("replay"));
    auto rebase = co_await reopened.RebaseAsync(topic);
    Require(rebase.report.status == git::WriteStatus::NeedsResolution, "Expected rebase conflict");
    auto recovered = Value(co_await service.OpenAsync(source));
    operation = Value(co_await recovered.ReadOperationAsync());
    Require(operation && operation->can_abort, "Linear rebase can be reopened");
    Write(fixture.repository / "notes.txt", "external edit after opening the conflict\n");
    auto stale_abort = co_await recovered.AbortAsync(operation->version, true);
    Require(stale_abort.report.issue && stale_abort.report.issue->code == git::ErrorCode::StaleSnapshot,
            "Abort must reject worktree changes after the review");
    operation = Value(co_await recovered.ReadOperationAsync());
    Succeeded((co_await recovered.AbortAsync(operation->version, true)).report);
    Require(Value(co_await recovered.ResolveRevisionAsync("HEAD")) == replay_head, "Abort must restore original HEAD");
    Require(!Value(co_await recovered.ReadOperationAsync()), "Abort must clear operation state");
    if (service.GetCapabilities().interactive_rebase) {
      const auto info = Value(co_await recovered.InspectAsync());
      const auto replayed = co_await recovered.InteractiveRebaseAsync(
          {topic, info.head, {{replay_head, git::RebaseAction::Drop}}}, {File(GIT_TEST_EXECUTABLE)});
      Succeeded(replayed.report);
      Require(replayed.report.backend == git::Backend::GitCli && replayed.commit == topic,
              "The selected Git executable must execute the reviewed rebase todo");
    }
#ifdef HUXERUI_GIT_TEST_HAS_LFS
    if (service.GetCapabilities().git_lfs) {
      RunGit(fixture.repository, "lfs install --local");
      RunGit(fixture.repository, "lfs track " + Quoted("*.bin"));
      Write(fixture.repository / "asset.bin", std::string(4096, '\0'));
      RunGit(fixture.repository, "add .gitattributes asset.bin");
      RunGit(fixture.repository, "commit -m lfs-asset");
      const auto lfs_info = Value(co_await repo.InspectAsync());
      auto job = Value(service.StartLfs(repo, {git::LfsAction::Push, "origin", {lfs_info.head.reference}},
                                       {File(GIT_TEST_EXECUTABLE)}));
      const auto uploaded = co_await job.WaitAsync();
      Succeeded(uploaded.report);
      Require(uploaded.report.backend == git::Backend::GitCli, "LFS must disclose its Git executable backend");
      auto objects = fixture.remote / "lfs" / "objects";
      Require(fs::exists(objects), "Git LFS must create the remote object directory");
    }
#endif
  } catch (const std::exception& error) {
    fixture.failure = error.what();
  } catch (...) {
    fixture.failure = "Unhandled integration fixture failure";
  }
  fixture.done = true;
}

void VerifyPreview(const fs::path& repository, Fixture* fixture = nullptr) {
#if defined(__APPLE__)
  constexpr KeyModifiers primary{.meta = true};
#else
  constexpr KeyModifiers primary{.control = true};
#endif
  const auto wide_line = "preview second edit " + std::string(180, 'x');
  if (fixture) {
    Write(repository / "notes.txt", Fixture::Text("preview first edit", wide_line));
  }
  preview_session = fixture ? std::optional{File((fixture->root / "preview.session").string())} : std::nullopt;
  const Application app{PreviewRoot,
                         {.viewport_breakpoints = {.medium_width = 690.0F, .expanded_width = 1080.0F},
                             .show_debug_overlay = false, .application_hooks = {git::Install}}};
  testing::UiTest ui(app, {.viewport = {1280.0F, 840.0F}, .resource_provider = std::make_shared<PreviewResources>()});
  const auto wait = [&](const testing::UiNodeQuery& query, std::string message) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!query.Exists() && std::chrono::steady_clock::now() < deadline) {
      ui.Pump();
      std::this_thread::sleep_for(1ms);
    }
    Require(query.Exists(), std::move(message));
  };
  const auto path = ui.Find(testing::UiSelector::Key("repository-path"));
  if (!path.Exists()) ui.Find(testing::UiSelector::Text("Enter path…")).Tap();
  wait(path, "Preview repository path field did not mount");
  constexpr testing::UiPointerOptions mouse{.device_kind = PointerDeviceKind::Mouse};
  const auto focus_path = [&] {
    ui.Pump();
    if (!path.One().focused) path.Tap(mouse);
    Require(path.One().focused, "The repository path editor must receive focus");
  };
  focus_path();
  path.ReplaceText(repository.string());
  const auto started = std::chrono::steady_clock::now();
  ui.Find(testing::UiSelector::Text("Open path")).Tap();
  wait(ui.Find(testing::UiSelector::Text("History")), "Preview did not open the repository");
  const auto history = ui.Find(testing::UiSelector::Key("history-list"));
  wait(history, "Preview did not mount the history list");
  const auto first = history.Find(testing::UiSelector::Text(fixture ? "baseline" : "Large history tip"));
  wait(first, "Preview did not render commit history");
  if (!fixture) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    ui.Pump();
    float distance = 0.0F;
    for (int page = 0; page < 6; ++page) {
      distance += history.ScrollBy({0.0F, 210.0F}).y;
      ui.Pump(16ms);
    }
    Require(distance > 0.0F && (!first.Exists() || !first.One().in_viewport),
            "Scrolling must leave the first history row");
    std::cout << "Large repository opened and first graph row rendered in " << elapsed.count() << " ms\n";
    return;
  }
  wait(ui.Find(testing::UiSelector::Text("Unified")), "Preview did not finish the initial review");
  auto initial = fixture->InspectGit("rev-list --max-parents=0 HEAD");
  if (const auto end = initial.find_first_of("\r\n"); end != std::string::npos) initial.resize(end);
  const auto revision_input = ui.Find(testing::UiSelector::Key("revision-input"));
  wait(revision_input, "The desktop history toolbar must show its revision search");
  ui.PressKey(Key::L, primary);
  ui.Pump();
  Require(revision_input.One().focused, "The revision shortcut must focus the toolbar input");
  revision_input.ReplaceText(initial);
  ui.Pump();
  const auto editor = revision_input.One();
  Require(editor.value == initial, "The revision editor must preserve its input, received " + editor.value.value_or("(none)"));
  Require(editor.focused, "Editing a revision must preserve focus");
  ui.PressKey(Key::Enter);
  wait(ui.Find(testing::UiSelector::Key("commit-hash:" + initial)), "Submitting a revision with Enter must open its commit");
  const auto commit_files = ui.Find(testing::UiSelector::Key("commit-files"));
  wait(commit_files, "Preview did not mount the commit file list");
  const auto commit_file = commit_files.Find(testing::UiSelector::Text("notes.txt"));
  wait(commit_file, "Preview did not show the selected commit's file list");
  wait(ui.Find(testing::UiSelector::Text("Unified")), "Preview did not open the first committed diff");
  Require(ui.Find(testing::UiSelector::Key("patch-pane")).One().bounds.width > 600.0F,
          "Desktop history must leave room to read the selected diff");
  Require(!ui.Find(testing::UiSelector::Text("Stage hunk")).Exists(), "Committed diffs must be read-only");

  const auto changes = ui.Find(testing::UiSelector::Key("workspace-changes"));
  wait(changes, "Preview did not finish reading the worktree");
  ui.Pump();
  changes.Tap(mouse);
  const auto unstaged = ui.Find(testing::UiSelector::Key("unstaged-files"));
  wait(unstaged, "Preview did not mount the changes workspace");
  const auto file = unstaged.Find(testing::UiSelector::Text("notes.txt"));
  wait(file, "Preview did not show the unstaged file");
  const auto stage = ui.Find(testing::UiSelector::Key("hunk-action-1"));
  wait(stage, "Preview did not render the selectable diff");
  const auto second_hunk = ui.Find(testing::UiSelector::Key("hunk-action-2"));
  wait(second_hunk, "Preview did not render the second hunk in the same diff");
  ui.Pump(16ms);
  const auto hunk_position = second_hunk.One();
  const auto patch_bounds = ui.Find(testing::UiSelector::Key("patch-pane")).One().bounds;
  Require(hunk_position.in_viewport, "Second hunk at " + std::to_string(hunk_position.bounds.x) + "," +
      std::to_string(hunk_position.bounds.y) + " in a diff at " + std::to_string(patch_bounds.x) + "," +
      std::to_string(patch_bounds.y) + " with width " + std::to_string(patch_bounds.width));
  const auto line = ui.Find(testing::UiSelector::Text(wide_line));
  const auto scroll = ui.Find(testing::UiSelector::Key("unified:notes.txt"));
  ui.Pump();
  const auto initial_x = line.One().bounds.x;
  const auto horizontal = scroll.ScrollBy({180.0F, 0.0F});
  ui.Pump();
  Require(horizontal.x > 0.0F && line.One().bounds.x != initial_x, "Long diff lines must scroll horizontally");
  scroll.ScrollBy({-180.0F, 0.0F});
  ui.Pump();
  stage.Tap(mouse);
  wait(ui.Find(testing::UiSelector::Text("Updated index")), "Preview did not report partial staging");
  const auto staged = fixture->InspectGit("show :notes.txt");
  Require(staged.find("preview first edit") != std::string::npos &&
          staged.find("preview second edit") == std::string::npos,
          "Preview must stage only the reviewed hunk");
  wait(ui.Find(testing::UiSelector::Text("1 hunk")), "Preview did not reload the remaining unstaged hunk");
  wait(stage, "Preview did not finish reloading the unstaged diff");
  ui.Find(testing::UiSelector::Text("Split")).Tap(mouse);
  const auto split = ui.Find(testing::UiSelector::Key("split:notes.txt"));
  wait(split, "Preview did not open the side-by-side diff");
  ui.Pump();
  ui.Pump(16ms);
  const auto hunk_x = stage.One().bounds.x;
  Require(split.ScrollBy({180.0F, 0.0F}).x > 0.0F, "Long lines must scroll in the split diff");
  ui.Pump();
  ui.Pump(16ms);
  const auto scrolled_x = stage.One().bounds.x;
  Require(std::abs(scrolled_x - hunk_x) < 1.0F,
          "Split view must keep hunk actions visible while scrolling. X moved from " + std::to_string(hunk_x) +
          " to " + std::to_string(scrolled_x));
  ui.Find(testing::UiSelector::Text("Unified")).Tap(mouse);
  wait(scroll, "Preview did not restore the unified diff");
  Require(stage.One().in_viewport, "Returning to the unified diff must keep the first hunk accessible");
  RunGit(repository, "commit -m " + Quoted("External preview commit"));
  ui.Find(testing::UiSelector::Text("Refresh")).Tap(mouse);
  ui.Find(testing::UiSelector::Key("workspace-history")).Tap(mouse);
  wait(ui.Find(testing::UiSelector::Text("External preview commit")), "Refresh must reopen updated history");
  Require(history.Find(testing::UiSelector::Text("External preview commit")).Exists(),
          "The external commit must appear in the history list");

  ui.SetWindowMetrics({.viewport = {390.0F, 844.0F}});
  ui.Pump();
  ui.Find(testing::UiSelector::Text("Changes")).Tap();
  wait(unstaged, "The compact layout did not show the working tree");
  file.Tap();
  const auto back = ui.Find(testing::UiSelector::Text("Back to changes"));
  wait(back, "The compact layout did not open the file inspector as a page");
  wait(stage, "The compact file inspector did not show the hunk action");
  back.Tap();
  wait(unstaged, "Returning from the compact inspector must restore the working tree");

  const auto conflict_repo = fixture->root / "preview-conflict";
  fs::create_directory(conflict_repo);
  RunGit(conflict_repo, "init --initial-branch=main");
  RunGit(conflict_repo, "config user.name " + Quoted("Preview Tests"));
  RunGit(conflict_repo, "config user.email preview@example.test");
  RunGit(conflict_repo, "config commit.gpgsign false");
  Write(conflict_repo / "review.txt", "base\n");
  RunGit(conflict_repo, "add review.txt");
  RunGit(conflict_repo, "commit -m base");
  RunGit(conflict_repo, "branch topic");
  Write(conflict_repo / "review.txt", "current\n");
  RunGit(conflict_repo, "commit -am current");
  RunGit(conflict_repo, "switch topic");
  Write(conflict_repo / "review.txt", "incoming\n");
  RunGit(conflict_repo, "commit -am incoming");
  RunGit(conflict_repo, "switch main");

  ui.SetWindowMetrics({.viewport = {1280.0F, 840.0F}});
  ui.Pump();
  ui.Find(testing::UiSelector::Text("Add repository")).Tap(mouse);
  if (!path.Exists()) ui.Find(testing::UiSelector::Text("Enter path…")).Tap(mouse);
  wait(path, "Adding a repository must open a new repository form");
  focus_path();
  path.ReplaceText(conflict_repo.string());
  ui.Find(testing::UiSelector::Text("Open path")).Tap(mouse);
  const auto topic = ui.Find(testing::UiSelector::Key("refs/heads/topic"));
  wait(topic, "The second repository's branch list did not open");
  wait(ui.Find(testing::UiSelector::Text("3 commits loaded")), "The second repository did not show its history");
  wait(ui.Find(testing::UiSelector::Text("Unified")), "The second repository did not finish its initial review");
  const auto bounds = topic.One().bounds;
  PointerEvent secondary{.type = PointerEventType::Down, .pointer_id = 73,
                         .position = {bounds.x + 22.0F, bounds.y + bounds.height / 2.0F},
                         .changed_button = PointerButton::Secondary, .pressed_buttons = PointerButton::Secondary};
  ui.SendPointer(secondary);
  secondary.type = PointerEventType::Up;
  secondary.pressed_buttons = PointerButton::None;
  ui.SendPointer(secondary);
  ui.Pump();
  const auto menu_merge = ui.Find(testing::UiSelector::Text("Merge into main…"));
  wait(menu_merge, "Secondary click did not open the branch menu");
  Require(ui.Find(testing::UiSelector::Text("3 commits loaded")).Exists(),
          "Secondary click must leave the history filter unchanged");
  ui.PressKey(Key::Escape);
  ui.Pump(250ms);
  topic.Tap(mouse);
  wait(ui.Find(testing::UiSelector::Text("2 commits loaded")), "The branch filter did not show its history");
  wait(ui.Find(testing::UiSelector::Key("unified:review.txt")), "The filtered branch must finish its review");
  ui.Pump(250ms);
  ui.FindSemantics(testing::UiSemanticSelector::Label("Actions for topic"))
      .PerformSemanticAction({SemanticActionKind::Activate, {}});
  wait(menu_merge, "The branch action button did not open the menu");
  ui.Pump(250ms);
  menu_merge.Tap(mouse);
  wait(ui.Find(testing::UiSelector::Text("Merge into main?")), "The merge must show a destination confirmation");
  ui.Pump(250ms);
  ui.PressKey(Key::O, primary);
  Require(ui.Find(testing::UiSelector::Text("Merge into main?")).Exists() && !path.Exists(),
          "Workspace shortcuts must yield to the merge dialog");
  ui.Find(testing::UiSelector::Text("Merge")).Tap(mouse);
  const auto conflict_files = ui.Find(testing::UiSelector::Key("conflict-files"));
  wait(conflict_files, "Merge must open the conflicts workspace");
  ui.Pump(250ms);
  const auto theirs = ui.Find(testing::UiSelector::Text("Use theirs"));
  wait(theirs, "The conflict inspector did not show the incoming resolution action");
  ui.FindSemantics(testing::UiSemanticSelector::Label("Use theirs"))
      .PerformSemanticAction({SemanticActionKind::Activate, {}});
  Require(Read(conflict_repo / "review.txt").find("<<<<<<<") != std::string::npos,
          "Choosing a text side must only populate the result editor");
  Require(ui.Find(testing::UiSelector::Value("incoming\n")).Exists(), "The selected side must be copied into the editor");
  ui.FindSemantics(testing::UiSemanticSelector::Label("Mark resolved"))
      .PerformSemanticAction({SemanticActionKind::Activate, {}});
  const auto proceed = ui.Find(testing::UiSelector::AllOf(testing::UiSelector::Text("Continue"),
                                                       testing::UiSelector::Enabled(true)));
  wait(proceed, "Resolving the last conflict must enable Continue");
  proceed.Tap(mouse);
  wait(ui.Find(testing::UiSelector::Text("Integration completed")), "Completing the merge was not reported");
  Require(Read(conflict_repo / "review.txt") == "incoming\n", "The merge must keep the reviewed incoming content");

  const auto session = fixture->root / "preview.session";
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline) {
    ui.Pump();
    if (fs::exists(session)) {
      const auto saved = git_preview::DecodeWorkspace(Read(session));
      if (saved && saved->tabs.size() == 2 && saved->active == saved->tabs[1].location &&
          saved->tabs[1].page != git_preview::WorkspacePage::Conflicts) return;
    }
    std::this_thread::sleep_for(1ms);
  }
  Require(false, "The Preview must save both tabs and the active worktree");
}

void VerifyPreviewRestore(Fixture& fixture) {
  const auto path = fixture.root / "preview.session";
  auto saved = git_preview::DecodeWorkspace(Read(path)).value();
  const auto linked = fixture.root / "preview-linked";
  RunGit(fixture.repository, "worktree add --detach " + Quoted(linked.string()) + " HEAD");
  saved.locations.push_back({"linked-worktree", linked.string(), "preview-linked"});
  saved.tabs.push_back({.location = "linked-worktree"});
  saved.locations.push_back({"missing-worktree", (fixture.root / "missing").string(), "preview-missing"});
  saved.tabs.push_back({.location = "missing-worktree"});
  saved.active = saved.tabs[0].location;
  auto& selected = saved.tabs[0];
  selected.page = git_preview::WorkspacePage::Changes;
  selected.path = "notes.txt";
  selected.split = true;
  const auto record = git_preview::EncodeWorkspace(saved);
  Require(git_preview::DecodeWorkspace(record) == saved && !git_preview::DecodeWorkspace(record + "garbage"),
          "Workspace records must round-trip and reject trailing corruption");
  Write(path, record);

  preview_session = File(path.string());
  const Application app{PreviewRoot,
                         {.viewport_breakpoints = {.medium_width = 690.0F, .expanded_width = 1080.0F},
                          .show_debug_overlay = false, .application_hooks = {git::Install}}};
  testing::UiTest ui(app, {.viewport = {1280.0F, 840.0F}, .resource_provider = std::make_shared<PreviewResources>()});
  const auto wait = [&](const testing::UiNodeQuery& query, std::string message) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!query.Exists() && std::chrono::steady_clock::now() < deadline) {
      ui.Pump();
      std::this_thread::sleep_for(1ms);
    }
    Require(query.Exists(), std::move(message));
  };
  wait(ui.Find(testing::UiSelector::Key("split:notes.txt")), "The active tab must restore the selected worktree patch");
  for (const auto& tab : saved.tabs) {
    Require(ui.Find(testing::UiSelector::Key("repository-tab:" + tab.location)).Exists(),
            "Linked worktrees and absent locations must retain their distinct tabs");
  }
  const auto unavailable = ui.Find(testing::UiSelector::Key("repository-tab:missing-worktree"));
  unavailable.Tap();
  wait(ui.Find(testing::UiSelector::Text("Locate…")), "An unavailable restored repository must offer recovery");
#if defined(__APPLE__)
  constexpr KeyModifiers primary{.meta = true};
#else
  constexpr KeyModifiers primary{.control = true};
#endif
  ui.PressKey(Key::O, primary);
  wait(ui.Find(testing::UiSelector::Text("Recent repositories")), "Open shortcut must show restored recent repositories");
  ui.PressKey(Key::W, primary);
  wait(ui.Find(testing::UiSelector::Text("Repository unavailable")), "Close shortcut must dismiss the add flow");
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--large-history") {
      VerifyPreview(argv[2]);
      return 0;
    }
    Fixture fixture;
    if (argc > 1) fixture.smart_remote = argv[1];
    const Application app{[]() -> View { return Text("Repository integration"); }, {
        .application_hooks = {[&](ApplicationContext&) { UseApplicationTaskScope().Launch(Verify(fixture)); }},
    }};
    {
      testing::UiTest ui(app);
      const auto deadline = std::chrono::steady_clock::now() + 30s;
      while (!fixture.done && std::chrono::steady_clock::now() < deadline) {
        ui.Pump();
        std::this_thread::sleep_for(1ms);
      }
    }
    Require(fixture.done, "Timed out waiting for repository operations");
    Require(fixture.failure.empty(), fixture.failure);
    VerifyPreview(fixture.repository, &fixture);
    VerifyPreviewRestore(fixture);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
