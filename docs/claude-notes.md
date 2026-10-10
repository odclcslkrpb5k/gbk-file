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
- Feature inventory: which Thunar features the user actually uses.

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

## Local toolchain (2026-10-10)
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
- Signal/source lifetime: g_signal_connect_object or disconnect in dispose; every
  g_idle/g_timeout id stored and g_clear_handle_id'd in dispose. Want a test/check for this.
- Git hooks via core.hooksPath (tracked .githooks/): pre-commit = build+tests,
  pre-push = analyzer + sanitizer test run. This replaces CI.
Protect (deliberate, flagged):
- -ftrivial-auto-var-init=zero, FORTIFY, stack protector: absorb residual bugs in release.
- Unavoidable exits: OOM (g_malloc aborts), X server connection loss. Not our code.
