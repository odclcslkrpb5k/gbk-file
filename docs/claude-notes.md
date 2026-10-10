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
- Event-driven fs change updates (files created by other apps appear without refresh).
- No thumbnails, ever.
- File type by filename extension only. No content sniffing.
  No extension -> default "unknown" type.

## Open questions
- Language/toolkit. Recommended C + GTK3 (Xfce GTK3 theme fidelity, cairo not GL,
  native API, gtk3-rs archived 2024). Waiting on: must it match Xfce/GTK3 theme exactly?
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
