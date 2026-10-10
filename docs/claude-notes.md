# claude-notes

Durable context for Claude. Read this first every session. Keep it current.

## Ground rules (from user, 2026-10-10)
- Filesystem writes: ONLY inside this repo. Nothing in ~/.claude memory, /tmp, etc.
- Durable context lives here (docs/claude-notes.md), not in Claude's memory system.
- Claude handles git. Push to main whenever appropriate. No branches/PRs (solo project).
- No CI. Builds are entirely local.
- Versions are tied to git hashes (no semver tags).

## Project
gbk-file: from-scratch Thunar clone for the user's own daily use. Linux / X11 / GTK / Xfce.
User has run Thunar ~10 years with an unchanged config and uses few of its features.

Primary goal: maximum performance in every scenario + minimal memory footprint.

## Decided
- C + GTK3 (decided 2026-10-10).
- Quality bar: "no crashes, ever". Use the Hierarchy of Defect Controls ladder
  (hierarchy-of-defect-controls plugin); every safety measure must be Engineer rung or
  better (Eliminate > Substitute > Engineer). Name the rung when proposing a control.
- Event-driven fs change updates (files created by other apps appear without refresh).
- No thumbnails, ever.
- File type by filename extension only. No content sniffing.
  No extension -> default "unknown" type.

## Open questions
- Extension edge cases (my proposals, not yet confirmed):
  1. dotfiles (.bashrc) = no extension -> unknown
  2. longest glob match (.tar.gz beats .gz), per shared-mime-info globs2
  3. case-insensitive
  4. no-extension + exec bit -> "executable" type (mode bits are free from statx)
- Config format: user partial to YAML; I recommended GKeyFile (.desktop-style) — awaiting answer.
- Double-click on executable: run directly, or ask? Scripts vs binaries?
- Delete key: trash (freedesktop trash spec) vs permanent; Shift+Del?
- View mode (details list / icons / compact), columns, default sort.
- Selection extras: shift/ctrl-click, rubber-band, type-ahead find? F2 rename?

## Feature inventory (user, 2026-10-10; "not exhaustive, muscle memory")
- F4: open terminal in cwd. Terminal command configurable (user uses xfce4-terminal).
- Ctrl+H: toggle hidden files.
- Alt+Left / Alt+Right: history back/forward. Alt+Up: parent dir.
- Arrow keys: move selection.
- Ctrl+C "and friends": copy/cut/paste etc. (clipboard must interop with other apps:
  x-special/gnome-copied-files + text/uri-list).
- Double-click: execute, or open with default handler (mimeapps.list via GAppInfo).
- Right-click item: action menu. Right-click empty space = menu for cwd as a folder.
- Configurable filetype-based menu actions (e.g. archive extraction). Uses this a lot.
- Plus from screenshot: tabs, button path bar w/ edit-location toggle, status bar, hamburger menu.
- Command execution rule (Eliminate shell injection): never pass through a shell; parse the
  template to argv once, substitute placeholders per-argument.

## Architecture notes (proposed, not final)
- Bypass GIO for listing: getdents64 + statx on worker thread, compact arrays.
- Custom virtualized view widget (cairo/Pango), not GtkTreeView/GtkIconView.
- Raw inotify fd on GLib main loop (g_unix_fd_add), batch events per frame; not GFileMonitor.
- Icons: per-type, rendered once per size, cached.
- Precomputed natural/casefold sort keys.
- Type table built from /usr/share/mime/globs2 at startup.

## User's current Thunar setup (docs/thunar.png)
- No menubar (hamburger button instead), no toolbar, no side pane.
- Path bar as buttons (home/gbk > Projects > gbk-file), pencil = edit location, computer icon at right.
- Tabs.
- Status bar: "1 folder | Free space: 40.9 GiB".
- Retro-ish theme (salmon/tan, bold monospace-ish labels) — theme fidelity likely matters.

## Build environment (decided 2026-10-10)
- Everything (build, tests, analyzers, fuzzers) runs in a Docker image defined in-repo.
  Workstation installs must not matter. Host has Docker 29.8 (rootless). Host = Ubuntu 26.04.1.
- Base image ubuntu:26.04 pinned by digest so the binary links against the same
  glibc/GTK as the host. Rebuild image when host distro upgrades.
- Build system: meson + ninja (proposed; user asked "make or alternative?").
- User is not a C programmer; relies on me for C good practice. Correctness/safety > convenience.

## Local toolchain (2026-10-10, host — only for reference now)
gcc 15.2 (default -std=gnu23, has -fanalyzer, -fhardened, -fzero-init-padding-bits),
glibc 2.43, GTK 3.24.52, GLib 2.88, meson + ninja.
NOT installed: clang, clang-tidy, valgrind, cppcheck, AFL++. (User must install; I can't write outside repo.)

## GTK3 status
3.24.52 (Mar 2026); upstream moved to one release/year, crash/important fixes only.
Stable target, no new features coming. Fine for us.

## Safety plan (proposed 2026-10-10, awaiting user OK)
Eliminate:
- No thumbnails/sniffing -> no decoder code in process. No plugins, no dlopen (-z nodlopen).
- File entries live in per-directory-load storage, referenced by (generation,index) handles,
  never raw pointers held across main-loop iterations -> stale-entry UAF can't exist.
- GTK touched only from main thread; workers send self-contained messages (owned copies).
- Filenames are opaque bytes; display strings made once via g_filename_display_name.
Substitute:
- gnu23: nullptr, constexpr, [[nodiscard]], {} init, <stdckdint.h> ckd_* for size math.
- GLib containers/strings; g_autofree/g_autoptr/g_steal_pointer/g_clear_*; G_DECLARE_FINAL_TYPE.
- GSlice is just malloc since GLib 2.76: use g_new/g_free, not g_slice.
Engineer:
- -Werror always (solo, never distributed). Big warning set incl. -Wconversion,
  -Wjump-misses-init (catches goto past g_auto vars), -Wshadow, -Wvla, -Wnull-dereference,
  -Wswitch-enum, -Wmissing-prototypes, -Wstrict-prototypes, -Wcast-qual, -Wformat=2.
- poison.h: #pragma GCC poison strcpy strcat sprintf vsprintf strncpy gets strtok atoi...
- Build variants: debug(ASan+UBSan, no FORTIFY), analyze(-fanalyzer, must be clean),
  release(OpenSSF 2026-08-20 TL;DR set + -fhardened-ish, FORTIFY=3, auto-var-init=zero).
- Tests run with G_DEBUG=fatal-warnings,fatal-criticals: any g_return_if_fail = failure.
- Stress tests: scanner vs. concurrent create/delete/rename storms; inotify overflow (IN_Q_OVERFLOW).
- Signal/source lifetime (user said close this gap, 2026-10-10):
  Eliminate: all signal connections + idle/timeout sources go through our wrapper module
  (owner-scoped; auto-disconnect/remove when owner OR instance is disposed, via weak refs).
  Engineer: lint script (runs in docker check stage) fails if raw g_signal_connect*,
  g_idle_add*, g_timeout_add*, g_source_attach appear outside the wrapper file.
  Engineer: debug builds keep a live-registration counter; tests assert zero at teardown
  and that callbacks never fire after owner dispose.
- NO git hooks / commit gating (user, 2026-10-10): repo contents don't matter, the BUILT
  binary does. Gate lives in the release build instead: it runs tests + analyzer + sanitizer
  suite and refuses to emit a binary unless all pass. Release builds refuse a dirty tree, so
  the embedded git hash fully identifies the source.
Protect (deliberate, flagged):
- -ftrivial-auto-var-init=zero, FORTIFY, stack protector: absorb residual bugs in release.
- Unavoidable exits: OOM (g_malloc aborts), X server connection loss. Not our code.
