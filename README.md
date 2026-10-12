# gbk-file

A small, fast file manager for Linux desktops (X11, GTK 3), modelled on
[Thunar](https://docs.xfce.org/xfce/thunar/start)'s compact view. It is written in C for
one person's daily use. The goals are to start instantly, stay instant in huge folders,
use little memory, and never crash.

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/odclcslkrpb5k/gbk-file/main/install.sh | bash
```

This installs the latest release into `~/.local`. It needs no root and copies two files:

- `~/.local/bin/gbk-file`
- `~/.local/share/applications/gbk-file.desktop`

Run the same command again to update. To make gbk-file open folders by default:

```sh
xdg-mime default gbk-file.desktop inode/directory
```

The installer takes these options, set as environment variables before `bash`. For
example, `curl … | GBK_VERSION=0b86673fc314 bash`.

| Variable | Effect |
| --- | --- |
| `GBK_VERSION=<hash>` | Install a specific release instead of the latest. |
| `GBK_PREFIX=<dir>` | Install under `<dir>` instead of `~/.local`. |
| `GBK_ANY_OS=1` | Skip the Ubuntu 26.04 check (see below). |

To uninstall, run `rm ~/.local/bin/gbk-file ~/.local/share/applications/gbk-file.desktop`.

**Supported systems.** Release binaries are built on **Ubuntu 26.04, x86_64**, against
that release's GTK and GLib. The installer refuses other systems rather than leave you a
program that won't start. On anything else, [build from source](#building).

## What it does

- **One view:** compact icon and name columns that scroll sideways, like Thunar's compact
  view. Sort by name, date modified or size, always with folders first.
- **Live updates:** the view follows changes on disk as they happen; there is no polling
  and no refresh button.
- **Big folders:** a folder of 100,000 files shows its first items within milliseconds and
  uses about 150 bytes per entry.
- **No thumbnails.** File types are decided by extension alone, so the program never reads
  a file's contents just to show the folder.
- **Tabs.** Each tab has its own path bar. Right-click a path bar button for that folder's
  menu.
- **Create files and folders** from the right-click menu on empty space.
- **Properties** (right-click menu or Alt+Enter): dates (modified, accessed, changed,
  created), size, owner, group, link target, and read / write / execute permissions you can
  change with Apply. For scripts, the right-click menu also has an **Executable** check
  item that switches execute permission on or off in one click.
- **Copy, cut, paste, trash, delete and rename.** File operations run in the background,
  with a progress window and Skip / Replace / Keep Both choices on conflicts. The clipboard
  works with other file managers.
- **Running files:**
  - Executable scripts run in a terminal that stays open, so you see their output.
  - Other executables run directly.
  - Everything else opens in its default application.
  - The right-click menu also offers Execute, Execute in Terminal and Open With.
- **Your own right-click actions** per file type, such as "Extract Here" for archives
  (see [Configuration](#configuration)).
- **Status bar:** folder and file counts, the selection's size and free space on the left.
  On the right are the sort button (click to cycle) and the `.*` hidden-files toggle.

### Keyboard

| Key | Action |
| --- | --- |
| Arrows, Page Up/Down, Home/End | Move; with Shift, extend the selection; with Ctrl, move without selecting |
| Space | Toggle selection of the item under the cursor |
| Ctrl+A | Select all |
| Typing | Jump to the first name starting with what you typed |
| Enter | Open |
| Alt+Left / Alt+Right / Alt+Up | Back / forward / parent folder |
| Alt+Home | Home folder |
| Mouse back / forward buttons | Back / forward |
| Ctrl+H | Show or hide hidden files |
| Ctrl+L | Type a location |
| F4 | Open a terminal here |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste |
| Delete | Move to trash |
| Shift+Delete | Delete permanently (asks first) |
| F2 | Rename |
| Alt+Enter | Properties (of the selected item, or the folder) |
| Menu key, Shift+F10 | Right-click menu |
| Ctrl+T / Ctrl+W | New tab / close tab |
| Ctrl+Page Up / Ctrl+Page Down | Previous / next tab |
| Ctrl+N / Ctrl+Q | New window / close window |

While you type in the location bar, keys go only to the text. Delete and Ctrl+V edit the
path there; they never act on your files.

## Configuration

The config file is `~/.config/gbk-file/gbk-file.conf`, in INI style. Every setting is
optional. [`docs/example.conf`](docs/example.conf) documents them all, and the test suite
loads it, so it always matches what the program accepts. You can set:

- the terminal commands (F4, and running scripts);
- whether hidden files are shown at startup;
- the starting sort;
- script interpreters;
- extra extension → file type mappings;
- right-click actions.

The file is read strictly. One unknown key or bad value rejects the whole file, and
gbk-file then reports every problem and uses the defaults, so a typo can't half-apply.

## Building

Everything builds inside a pinned Docker image, so the only host requirements are Docker
and bash. Nothing is fetched during a build, and versions are git hashes.

```sh
./build debug     # AddressSanitizer + UBSan build, all tests
./build analyze   # GCC static analyzer; any finding fails
./build fuzz 60   # libFuzzer on every parser, 60 s each
./build bench     # scan / sort / memory benchmark
./build release   # all of the above (20 s fuzzing), then the hardened build and its tests
./build install   # install the release build into ~/.local (runs release if needed)
./build publish   # upload the release build as a GitHub release (maintainer)
```

`release`, `install` and `publish` refuse to run with uncommitted changes, so a version
hash always names exact source.

The build environment is Ubuntu 26.04 with GCC 15, GTK 3.24, GLib 2.88 and Meson.
Building natively without Docker should work with those or newer versions (Meson ≥ 1.3,
a C23 compiler). That path is not tested:

```sh
mkdir -p out && git rev-parse --short=12 HEAD > out/VERSION
meson setup out/native -Dbuildtype=debugoptimized -Db_ndebug=false -Dhardening=true
meson compile -C out/native && meson test -C out/native
```

### How it avoids crashing

These are checks the build enforces, not guidelines:

- Unsafe C library functions, blocking GTK calls and raw callback registration are
  *poisoned*: using them is a compile error, and the build proves each ban still bites.
- Every signal handler and main-loop callback is tied to an owner object, so no callback
  can fire into a destroyed window. A counter of live registrations must return to zero
  when the program exits.
- All filesystem access for a folder happens on a worker thread, so a hung network mount
  can't freeze the window.
- File operations never overwrite in place: they write a temp file, fsync it and rename
  it over the target. Creation and renames refuse to replace existing files. Recursive
  delete never follows symlinks out of the tree being deleted.
- Tests run under AddressSanitizer and UBSan with GLib warnings made fatal. The parsers are
  fuzzed, and GCC's static analyzer must report nothing.
