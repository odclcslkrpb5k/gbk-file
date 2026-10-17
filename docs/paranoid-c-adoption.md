# Adopting Paranoid C (GTK 3) in Quoin

A record of applying the draft appendix
[Paranoid C (GTK 3)](https://github.com/odclcslkrpb5k/paranoid-typescript/blob/main/appendices/c-gtk.md)
and the core rules it cites to Quoin, 2026-10-16/17: what was done, what each control
found, where the appendix was right, wrong or silent. Each step is one commit; the commit
messages carry the detail. Quoin already had a strict regime of its own (`docs/claude-notes.md`),
so much of the appendix was met before step 1; the useful results are where it was not.

Rule IDs (CP-1, ER-6, ...) are the core standard's.

## Toolchain actually used

Ubuntu 26.04 base image pinned by digest; since step 13 every package from the archive
snapshot of 2026-10-16. Versions:

| Tool | Version |
|---|---|
| GCC | 15.2.0 (Ubuntu 15.2.0-16ubuntu1) |
| Clang, clang-tidy, clang-format (scan-build, measured then dropped) | 21.1.8 (1:21.1.8-6ubuntu1) |
| cppcheck | 2.19.0 |
| Coccinelle (measured then dropped) | 1.3 |
| GTK 3 / GLib / glibc | 3.24.52 / 2.88.0 / 2.43 |
| Meson / Ninja / Python | 1.10.1 / 1.13.2 / 3.14.4 |
| binutils (readelf) | 2.46 |
| desktop-file-validate | 0.28 |
| apt | 3.2.0 |
| Docker / Compose | 29.8 / 5.6.0 |

The appendix was checked on Debian trixie with GCC 14.2, Clang 19.1, GTK 3.24.49, GLib 2.84,
cppcheck 2.17: every tool here is newer.

## The steps

| # | Commit | What |
|---|---|---|
| 1 | 470e2f0 | compose.yaml; `./build check` = every check (TC-9, TC-5) |
| 2 | 17e475b | a fixture per warning flag; G_DISABLE_* refused; binary checked (CP-1, CP-3, ER-2) |
| 3 | 3444a83 | GCC's analyzer taught GLib's allocators |
| 4 | 68035b2 | clang-tidy + cppcheck behind a shrinking baseline (ch. 03, AD-2) |
| 5 | 69eded6, cfddf83 | fallible functions `[[nodiscard]]`; my unsequenced-read bug and its rule (LN-6) |
| 6 | 200f5af | discarded failures listed and refused (ER-6) |
| 7 | 02780c8 | stall watch: a callback blocking the main loop fails the tests (UI thread) |
| 8 | c546c5e | no suppressions, no `(void)` casts on calls (LN-8) |
| 9 | 5efc339 | no paths in logs, no branching on messages, no hidden characters (DP-3, ER-8, CD-7) |
| 10 | beada4e | `-Wsuggest-attribute=format`; desktop entry validated |
| 11 | c21fab5 | baseline burned down from 81 to 10 (AD-4 step 6, CD-8) |
| 12 | 7628a89 | clang-format, checked (TC-6) |
| 13 | 7d1b5af | apt snapshot pinning; unused tools out of the image (TC-1, TC-3) |

## Numbers

Size at the start: 11,564 lines of C in `src/`, 6,536 in `tests/`; 263 `g_autofree`/
`g_autoptr`/`g_auto`, 73 explicit `g_free`, 28 `switch`, 38 `[[nodiscard]]`, 8 `(void)` casts
on calls, 0 suppression comments.

**Warning flags.** All of the appendix's list except the deprecation macros for GDK were
already on, with zero warnings. Of Quoin's 23 flags, 8 did nothing on GCC 15 / C23
(step 2). `GDK_VERSION_MIN_REQUIRED/MAX_ALLOWED=3.24`: 0 warnings. `-Wsuggest-attribute=format`:
0.

**Analyzers on the existing code, raw:**

| Tool | Findings | Real defects | Notes |
|---|---|---|---|
| clang-tidy (bugprone, cert, clang-analyzer) | ~790 | 0 | 605 one check (GObject casts) |
| cppcheck 2.19 | ~180 (+ 2 files unparsed, 1 file unparseable) | 0 | 88 const-correctness |
| scan-build | 11 | 0 | all 11 also clang-tidy's |
| GCC -fanalyzer, untaught | 0 | 0 | it was already in the gate |
| GCC -fanalyzer, taught GLib | ~35 | 0 | 20 leak FPs, 2 refcount, 1 nullable model |
| check-source rules (new) | 13 sites | 1 (fixed) | merge EACCES via the NULL-GError listing |

**Baseline:** 81 entries when taken (step 4), 80 after step 6, 10 after step 11; 9 of those
can never be fixed (GObject `_init` signatures; `g_atomic_int_get` casting away const) and 1
is cppcheck's parse failure on `tests/test-window.c`, kept visible.

**Mutants planted to prove controls** (all caught): 5 GLib memory bugs (taught analyzer),
dead store (clang-tidy and cppcheck), new/fixed/raised baseline entries (gate), dead flag
(fixture test), 300 ms sleeps in a source and a handler (stall watch), extra `(void)` cast,
NULL-GError discard, unformatted function, broken desktop entry ×2.

**ASan/UBSan, LeakSanitizer and the fuzzers** ran in every one of 13 full checks and found
nothing new during this work. They had found real bugs earlier in the project (see the
notes: a rescan that reused a directory offset, caught by a storm test under ASan).

## Defects found

1. **Folder merge invented its error** (`fileops.c`, "Replace" of a folder over a folder).
   If the source folder could not be opened, the move reported `EACCES` whatever the cause.
   Found by listing every NULL `GError` argument (step 6) and reading each of the 19 sites
   in `src/`. No analyzer reports it; no test covered it, and none fails without the fix
   (any other errno from `opendir` there needs fault injection). A careful review of that
   function would have caught it.
2. **Unsequenced read in my own step-5 change** (`job.c`): `agree (run (&err), err)` reads
   `err` before `run` writes it, as GCC orders arguments. The existing tests caught it
   (two suites aborted). I pushed it anyway: I read the exit status of the shell wrapping
   the check, not the check's. No compiler warning or analyzer reports this shape (checked:
   GCC `-Wsequence-point` and `-fanalyzer`, Clang `-Weverything` incl. `-Wunsequenced`,
   clang-tidy bugprone/cert/misc/analyzer, cppcheck `--enable=all`). `check-source.py`'s
   `unsequenced` rule now does.
3. **Four log/abort messages carried a path** (DP-3): one `g_warning` that reaches the
   journal, three aborts. Found by grep, then a rule.

No defect in shipped behaviour came from a compiler flag or an analyzer. As in the core
standard's measured finding, the behavioural defect came from reading code that a machine
listing pointed at, and the regression from a test.

## Appendix section by section

Verdicts: **as written**, **changed** (what and why), **wrong**, **n/a**.

### Memory safety

- *Eliminate: g_autoptr / g_autofree / g_steal_pointer / g_clear_*.* **As written** (in
  use from the start). Ownership in GI terms (`transfer full`) in header comments:
  **changed** — Quoin's headers say "owned"/"borrowed" in words; no checker reads either.
- *Substitute: GLib containers, g_new overflow check.* **As written.**
- *Engineer: `#pragma GCC poison` list.* **Changed**: the list is far too short and the
  mechanism has two traps. Quoin poisons also `strncpy strncat strtok atoll atof popen
  g_spawn_command_line_* alloca g_newa g_alloca g_error pthread_cancel pthread_exit
  g_thread_exit gtk_dialog_run gtk_main g_main_loop_run gtk_clipboard_wait_*` and every
  raw signal/source API outside one file. Traps: (1) system headers must all be included
  before the poison (a force-included prelude does it); (2) a macro (g_error, alloca,
  g_signal_connect) must be `#undef`d before poisoning, or uses through it are not caught.
  Each ban gets a configure-time probe that must fail to compile.
- *Engineer: ASan/UBSan with `_FORTIFY_SOURCE` off.* **As written**, plus: on Ubuntu,
  GCC defines `_FORTIFY_SOURCE=3` by itself whenever optimising, so a sanitizer build at
  -O1 or above needs an explicit `-U_FORTIFY_SOURCE`.
- *"-fanalyzer is blind to GLib's allocators".* **Wrong as a fixed fact.** GCC's
  `malloc (deallocator, index)` attribute teaches it: redeclare `g_malloc`, `g_malloc_n`,
  `g_strdup`, `g_strdup_printf`, `g_build_filename`, ... with
  `__attribute__ ((malloc, malloc (g_free, 1), malloc (g_free_sized, 1), returns_nonnull))`
  and `g_strsplit` with `malloc (g_strfreev, 1)` (`src/analyzer-glib.h`). Details that
  matter: `g_free` is a macro choosing `g_free_sized` when the size is known, so both
  deallocators are needed; names must be parenthesised (`(g_strdup)`, a macro); do it only
  in the analyzer build, because `returns_nonnull` is an optimiser promise and
  `g_malloc (0)` returns NULL; `#undef g_strdup` there (GLib's inline wrapper confuses the
  analyzer). Mutant matrix, same file, six GLib bugs (double g_free; use after g_free;
  g_autofree plus explicit g_free; g_strsplit freed with g_free; g_malloc freed with free;
  leak on an early return):

  | | GCC untaught | GCC taught | Clang analyzer | cppcheck --library=gtk |
  |---|---|---|---|---|
  | caught | 0/6 | 6/6 | 3/6 | 0/6 |

  The attribute also turns on the ordinary compiler warning `-Wmismatched-dealloc`.
- *Protect: hardening.* **As written**; Quoin adds `-fzero-call-used-regs=used-gpr` and
  GCC 15's `-fzero-init-padding-bits=all`. `-Wl,-z,nodlopen` is a no-op for an executable.
- *Lifetimes in callbacks: g_signal_connect_object, g_clear_handle_id, GCancellable.*
  **Changed**: Quoin uses a higher rung the appendix should name. One module
  (`lifetime.c`) is the only place allowed the raw APIs (all poisoned elsewhere); every
  connection and source has an owner and is removed when the owner is disposed or the
  instance finalized; a live-registration counter is asserted zero at test teardown. It
  caught a leaked GtkMenu (a toplevel is not destroyed with its attach widget). Async
  operations hold a `GWeakRef` to their owner and drop the result if it has gone.

### One UI thread

- **As written**, and the missing Engineer control now exists (step 7): every callback of
  ours timed; `GBK_STALL_MS=250` in tests aborts naming the callback. Measured under ASan:
  worst 85 ms. The design point: time callbacks, not the poll loop (poll gaps include test
  code between iterations). GLib allows one pair of marshal guards per closure and
  `g_object_watch_closure` takes it, so a meta-marshal around `g_cclosure_marshal_generic`
  does the timing, installed only when the watch is on.
- Missing hazard: GIO and GTK calls that block on D-Bus synchronously on the main thread
  (`g_volume_monitor_get`, about 20 ms; `gtk_mount_operation_new`). Not "file or network
  I/O" by name, but the same freeze.
- Missing Engineer control still: a GTK call from a worker thread. (Quoin: workers own no
  GTK objects by construction; no check.)

### Toolchain (ch. 01)

- *TC-1/TC-9 digest-pinned image.* **As written.** Compose adds nothing technical for a
  one-container project without CI (the user's rule: no CI), but makes the environment
  declarative; `./build` drives it.
- *apt pinned via snapshot.debian.org.* **Changed** for Ubuntu: `snapshot.ubuntu.com` and
  `apt-get update --snapshot <date>` (apt ≥ 2.7). Traps: the base image has no CA
  certificates and the service is HTTPS-only (install `ca-certificates` from the live
  archive first); `apt-get update` exits 0 when indexes fail to download, and the install
  then uses the subset it got (seen: security and backports missing, gcc installed
  anyway) — use `--error-on=any`; a future date is not refused (the newest snapshot is
  served).
- *TC-3 Meson wraps.* **N/A** (no wraps).
- *TC-5 one check command.* **As written** (`./build check`).
- *TC-6 clang-format.* **As written**; tuned config still reformatted 3.4% of lines.
  `AlignTrailingComments: Leave` and `AlignEscapedNewlines: DontAlign` are what make GTK
  code's aligned field comments and macros survive.
- *Compiler pinned with the image.* **As written.**

### Compiler settings (ch. 02)

- *The warning list.* **Changed.** `-std=c17` contradicts the appendix's own advice to
  use C23's `ckd_add`; Quoin uses `gnu23`. Under C23 on GCC 15, eight of the appendix's
  (and Quoin's) flags do nothing: `-Wstrict-prototypes` (dead: `int f()` is a prototype
  in C23, in both compilers), `-Wsign-conversion` (in `-Wconversion`, both compilers —
  confirmed), `-Wimplicit-fallthrough` (in `-Wextra`), `-Wmissing-declarations` (same as
  `-Wmissing-prototypes` in C), `-Wpointer-arith` (in `-Wpedantic`), and
  `-Werror=implicit`, `-Werror=incompatible-pointer-types`, `-Werror=int-conversion`
  (errors by default since GCC 14, and `werror` covers them anyway). `-Wbidi-chars=any`:
  GCC 15's default already rejects paired controls and marks. Found only because each flag
  got a fixture that must fail with it and pass without it.
- *Deprecation macros.* **As written** (GLib already; GDK added, 0 warnings).
- *Hardening list.* **As written.**
- *CP-2 everything built the same way.* **Changed**: fuzz targets need Clang (libFuzzer),
  which lacks GCC's extra warnings; they build with the common subset.
- *CP-3 readelf.* **Changed — weaker than it looks on Ubuntu.** Ubuntu's GCC enables PIE,
  `-z now`, RELRO, `-fstack-protector-strong`, `-fstack-clash-protection` and
  `-fcf-protection` by default; the unhardened debug binary passed every readelf property
  but FORTIFY. The stronger check: GCC records each compilation unit's options in
  `DW_AT_producer` (`-grecord-gcc-switches`, default), so with `-g` every unit can be
  required to carry each flag (`tools/check-binary.sh`; the debug build fails 10 checks).
  Practical: about 40 lines of bash.
- *"One fixture per flag that must fail to compile".* **As written, and the most useful
  item in the chapter** — it found the 8 dead flags. Traps: Meson's `cc.compiles()` omits
  `warning_level`'s flags; GCC's `-fsyntax-only` skips the passes that emit some warnings
  (`-Wimplicit-fallthrough`); some need `-O2`.

### Static analysis (ch. 03)

- *Four analyzers, each finds what the others miss.* **Changed.** On this codebase:
  scan-build is clang-tidy's `clang-analyzer-*` (11 of 11 findings shared) — run one.
  clang-tidy and cppcheck found no defects; cppcheck's unique value was const-correctness
  (CD-8). The taught GCC analyzer beats both on GLib memory. Each needs heavy
  configuration for GLib C; the reasons are in `.clang-tidy` and
  `tools/cppcheck-suppressions.txt`: casting-through-void (GObject casts, 605),
  reserved-identifier (inside GLib macros), multi-level-implicit-pointer-conversion
  (g_steal_pointer), assignment-in-if-condition (GLib macros), unix.Malloc (never runs
  cleanup functions: every g_autofree is a "leak"), EnumCastOutOfRange (GTK flag enums),
  insecureAPI (Annex K), valist.Uninitialized (Clang 21 does not model C23's
  `__builtin_c23_va_start`), assertWithSideEffect, constParameterCallback.
- *Fail-open tools* — the appendix's biggest omission; a gate must parse reports and prove
  coverage, never trust exit codes:
  - `scan-build --status-bugs` exits 0 with `-plist` or `-sarif` whatever it found;
  - cppcheck given a directory as `--project`, or an unknown `--library`, checks nothing,
    writes nothing, exits 0;
  - cppcheck stops analysing a file at an unknown macro (`G_GNUC_PRINTF`,
    `G_GUINT64_FORMAT`; fixed with a small library file) and cannot parse a file that
    includes GTK and X11 headers together;
  - `apt-get update` (above).
- *LN-2 escape hatches.* **As written** (`-Wcast-qual`; `G_DISABLE_CAST_CHECKS` now refused).
- *LN-3 -Wswitch-enum livable?* **Confirmed livable**: on since the first commit, 28
  switches, no suppressions. Gap: GTK dialog responses are `int`, so a response switch is
  not covered.
- *LN-5 gap.* **Confirmed**: clang-tidy 21's `readability-implicit-bool-conversion` is
  silent on `if (p)` and `if (n)` in C.
- *LN-6 `(void)`.* **Confirmed, with a hole**: GCC: `(void)` silences `[[nodiscard]]` but
  not `warn_unused_result`; Clang: silences both. But `(void) !f ()` silences
  `warn_unused_result` in GCC too, so preferring `G_GNUC_WARN_UNUSED_RESULT` buys little;
  refusing the cast (both spellings) is what holds. Quoin keeps `[[nodiscard]]`, and a
  rule requires it on every function taking `GError **` that returns a value.
- *LN-8 suppressions.* **As written**, as a script; exceptions per file with exact count.
- *LN-9 Coccinelle/semgrep.* **Changed.** Coccinelle 1.3 could not resolve GLib's
  prototypes (it does not expand `GLIB_AVAILABLE_IN_ALL`-style macros), and Ubuntu's spatch
  cannot run Python rules without libpython. What worked: preprocess the prelude with
  `gcc -E`, read every prototype with a `GError **` parameter (676), and check calls with a
  small argument splitter (`tools/check-source.py`, self-tested).

### Errors (ch. 04)

- *Return value and error agree.* **As written**; the job worker now checks it at runtime
  (and my first version of that check was the unsequenced bug).
- *ER-6 NULL GError.* **Changed to an Engineer control**: every NULL GError listed; the
  case with no trace at all (NULL error *and* result discarded) refused (2, in tests). The
  listing found defect 1.
- *ER-8.* **As written**; a rule now refuses `->message` in comparisons (0 existed).
- *ER-3, ER-4.* **As written** (errno → GError at once; `atoi` poisoned;
  `g_ascii_string_to_*`).
- *ER-2 "call g_error, which always aborts".* **Wrong with -fanalyzer**: GCC 15.2 still
  reports `g_error`'s trailing `for (;;)` as `-Wanalyzer-infinite-loop`. Use a project
  `[[noreturn]]` function that calls `abort` (Quoin: `gbk_fatal`, with `G_GNUC_PRINTF`),
  and poison `g_error`.
- *G_DISABLE_ASSERT.* **Confirmed, corrected detail**: `g_assert_not_reached ()` becomes
  `__builtin_unreachable ()`. In my probe it did not segfault: GCC -O0/-O2 and Clang -O2
  silently ran into the next `case` and returned its value; Clang -O0 returned garbage
  (28746). `g_assert` vanishes; the `g_assert_*` test macros (`g_assert_true`, ...) do
  **not** — they abort regardless. Quoin's prelude now `#error`s on G_DISABLE_ASSERT,
  G_DISABLE_CHECKS and G_DISABLE_CAST_CHECKS (probed at configure).
- *ER-7 g_variant_get.* **N/A** in practice: Quoin's only GVariants are action
  parameters, whose type GAction checks before activation.
- *ER-5 counted log writer.* **Not adopted**: a desktop application has no consumer for
  the count; GLib's default writer already sends every warning to the journal with its
  domain. Tests make warnings fatal.
- *ER-9.* **As written** (abort, core dump).
- *ER-10.* **As written** (cancelled operations are not reported as errors).

### Boundaries (ch. 05)

- *BD-1 config via GKeyFile typed getters.* **As written** (all-or-nothing load). GSettings,
  json-glib, gdbus-codegen: **n/a**.
- *Text: g_utf8_validate / g_filename_display_name.* **As written.**
- *Format strings: G_GNUC_PRINTF on own variadics.* **Changed to Engineer**: GCC's
  `-Wsuggest-attribute=format` makes a missing attribute a build error (Clang has no
  equivalent). `msgfmt --check`: **n/a** (no translations).
- *BD-2 coercions.* Not inventoried beyond `-Wconversion`/`-Wcast-qual`; no tool here
  lists casts for review.
- *BD-4 integer sizes.* **As written** (`ckd_*`, `g_new`). clang-tidy's
  implicit-widening check found 6 int multiplications widened to 64 bits (constants; no
  overflow possible); fixed with typed constants.
- *BD-5 single-member-struct brands.* **Not adopted**: two id kinds (`GbkHandle`,
  `GbkTypeId`) of different widths; cost would be every handle use.
- *BD-6.* **As written** (display only, local zone).

### Code (ch. 06)

- *CD-7.* **Changed**: GCC 15 already rejects bidirectional controls by default; the
  repository scan for the rest (no-break, zero-width, separators) is a rule now, over C
  and the files the compiler never reads.
- *CD-8 const.* **Changed**: cppcheck is the only tool that finds missing const, and
  applying it is iterative (each const exposes more). It cannot be complete for GObject
  code: `G_DEFINE_TYPE` fixes `_init` signatures, and `g_atomic_int_get` casts const away,
  so `-Wcast-qual` refuses a const pointer to an atomic field.
- *Subprocesses.* **As written** (argv only; shell APIs poisoned).

### Testing (ch. 07)

- *GTest/TAP, fatal criticals.* **As written.**
- *TS-3 leaks: "a GTK suppression file, about 100 KB in 482 allocations".* **Wrong on
  these versions**: one window created and destroyed leaks 21,656 bytes in 690 allocations
  under LSan, and every byte is fontconfig's cache. One line, `leak:libfontconfig.so`,
  suppresses all of it, and Quoin's whole GUI suite passes LSan with only that line. A
  broad GTK suppression file would hide the application's own GObject leaks.
- *TS-16 fuzz every parser.* **As written** (7 libFuzzer targets; seeds replayed as tests).
  theft: not used.
- *TS-5 Mull.* **Not tried**: not packaged for Ubuntu 26.04 / LLVM 21. Quoin mutation-checks
  by hand: plant the bug, run the suite, restore — every control in this work.
- *gtk-builder-tool validate under xvfb catches real .ui mistakes?* **Partly.** It needs a
  display (confirmed). On 11 broken files it caught 6 (unknown property, class and signal,
  dangling id, duplicate id, malformed XML) and passed 5 (bad int value, bad enum value,
  two children in a GtkBin, unknown packing property, duplicate property). Loading each
  file with `gtk_builder_add_from_file` under `G_DEBUG=fatal-warnings` caught 10 of 11
  (all but the duplicate property): recommend that load test instead. Quoin has no `.ui`
  files (**n/a** for adoption).
- *desktop-file-validate.* **As written**, in the check. appstreamcli: **n/a** (no metainfo).
- *Test environment hazards the appendix misses*: GTK's mount-operation dialog needs a
  session bus (a private `dbus-daemon` in the container); no a11y bus
  (`NO_AT_BRIDGE=1`); `GSETTINGS_BACKEND=memory`; a fake GIO error code 0 is
  `G_IO_ERROR_FAILED`, not "no error"; GTK can start a drag from synthetic events but not
  drive one (XTest needed).

### Data protection (ch. 11)

- *DP-1.* **N/A** (no secrets).
- *DP-3.* **As written**; four violations found and fixed, and a rule for `src/` (tests
  log their own temporary folders).

## What the appendix misses

1. GCC's analyzer can be taught GLib (above) — the single biggest gain of this work.
2. Fail-open tools (above). Every gate must parse the report and prove what it covered.
3. Unsequenced reads around out-parameters: `f (g (&x), x)`. No compiler or analyzer flags
   it; a textual rule does.
4. Owner-scoped registration (Eliminate) instead of `g_signal_connect_object` (Engineer).
5. Nested main loops (`gtk_dialog_run`, `g_main_loop_run`, `gtk_clipboard_wait_*`) as a
   reentrancy hazard: poison them; use async responses.
6. The poison mechanism's traps (header order, macros) and per-ban configure probes.
7. Ubuntu's compiler defaults defeat readelf checks; `DW_AT_producer` does not.
8. `-std=c17` vs C23's `ckd_add`; under C23 several listed flags are dead.
9. Clang's analyzer and cleanup attributes; Clang 21 and C23 `va_start`.
10. A stall watch built at the one place callbacks are registered.
11. apt snapshot traps on Ubuntu.
12. `g_atomic_int_get` and const; `G_DEFINE_TYPE` and const — CD-8 cannot be complete.

## Which recommendations Quoin proves

Proven here (used, mutation-checked where a check exists, on a real codebase):
- the warning set (with the C23 corrections), `werror`, and one fixture per flag;
- poisoning banned functions, with configure probes;
- ASan/UBSan/LSan in the test build, with fontconfig as the only suppression;
- refusing `G_DISABLE_ASSERT/CHECKS/CAST_CHECKS`;
- `-fanalyzer` in the gate — **taught** GLib's allocators;
- clang-tidy and cppcheck behind a shrinking baseline (with the reasoned exclusions);
- `[[nodiscard]]` on fallible functions plus refusing `(void)` casts;
- LN-8 as a script with per-file counted exceptions;
- `-Wswitch-enum`;
- fuzzing every parser;
- desktop-file-validate;
- clang-format checked in the gate;
- hardening flags, checked per compilation unit;
- DP-3 for logs.

Not proven (not adopted, or n/a here): GI ownership annotations, `g_signal_connect_object`
(superseded), counted log writer, single-member-struct brands, Mull, theft,
gtk-builder-tool validate (replace with a load test), msgfmt, appstreamcli, GSettings,
json-glib, libsecret.
