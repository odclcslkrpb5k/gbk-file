project('quoin', 'c',
  # The git hash, written by tools/in-container.sh before every setup. One source of truth:
  # meson's summary, --version and the binary all read it.
  version : files('out/VERSION'),
  meson_version : '>= 1.3',
  default_options : [
    'c_std=gnu23',
    'warning_level=3',
    'werror=true',
    'b_pie=true',
  ],
)

cc = meson.get_compiler('c')

# Toolchain is pinned by the Docker image, so every flag here is required, not probed:
# an unsupported flag should fail the build rather than silently weaken it.
add_project_arguments(
  # Every translation unit starts with the prelude (system headers, then poisoned APIs).
  '-include', meson.project_source_root() / 'src' / 'prelude.h',

  # Warnings (all are errors via werror).
  '-Wformat=2', '-Werror=format-security',
  '-Wconversion', '-Wsign-conversion',
  '-Wimplicit-fallthrough',
  '-Wshadow',
  '-Wvla',
  '-Wnull-dereference',
  '-Wswitch-enum',
  '-Wmissing-prototypes', '-Wstrict-prototypes', '-Wmissing-declarations',
  '-Wcast-qual',
  '-Wpointer-arith',
  '-Wwrite-strings',
  '-Wundef',
  '-Wdouble-promotion',
  '-Werror=implicit', '-Werror=incompatible-pointer-types', '-Werror=int-conversion',

  # Semantics: never let the optimiser exploit UB we might have.
  '-fno-strict-aliasing',
  '-fno-strict-overflow',
  '-fno-delete-null-pointer-checks',
  '-fstrict-flex-arrays=3',


  '-D_GNU_SOURCE',                 # statx, O_* flags
  '-DG_LOG_DOMAIN="gbk"',
  '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_88',
  '-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_88',
  '-DGDK_DISABLE_DEPRECATED',
  '-DGTK_DISABLE_DEPRECATED',
  language : 'c',
)

# GCC is the compiler for every shipped/tested build. Clang is used only for libFuzzer
# builds, so GCC-only warnings are skipped there (clang has no equivalent).
if cc.get_id() == 'gcc'
  add_project_arguments(
    '-Wjump-misses-init',            # goto/switch skipping a g_auto* initialisation
    '-Wcast-align=strict',
    '-Wtrampolines',
    '-Wbidi-chars=any',
    '-Wduplicated-cond', '-Wduplicated-branches', '-Wlogical-op',
    language : 'c',
  )
endif

if get_option('fuzzing')
  assert(cc.get_id() == 'clang', 'fuzzing builds need clang (libFuzzer)')
  add_project_arguments('-fsanitize=fuzzer-no-link', language : 'c')
endif

if get_option('hardening')
  # OpenSSF Compiler Options Hardening Guide (2026-08-20), production set for x86_64.
  # Not used in sanitizer builds: FORTIFY interferes with ASan.
  add_project_arguments(
    '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3',
    '-fstack-clash-protection', '-fstack-protector-strong',
    '-fcf-protection=full',
    '-fzero-call-used-regs=used-gpr',
    '-ftrivial-auto-var-init=zero',
    '-fzero-init-padding-bits=all',
    language : 'c',
  )
  add_project_link_arguments(
    '-Wl,-z,nodlopen', '-Wl,-z,noexecstack',
    '-Wl,-z,relro', '-Wl,-z,now',
    '-Wl,--as-needed', '-Wl,--no-copy-dt-needed-entries',
    language : 'c',
  )
endif

if get_option('analyzer')
  add_project_arguments('-fanalyzer', language : 'c')
else
  # OpenSSF: -fexceptions so pthread cancellation unwinds through C frames. We ban
  # cancellation outright (prelude.h), so nothing can unwind; the analyzer build leaves it
  # out because with it, -fanalyzer reports every resource as leaked on unwind paths that
  # cannot happen here.
  add_project_arguments('-fexceptions', language : 'c')
endif

conf = configuration_data()
conf.set_quoted('GBK_VERSION', meson.project_version())
configure_file(output : 'gbk-config.h', configuration : conf)
config_inc = include_directories('.')

glib_dep = dependency('glib-2.0', version : '>= 2.88')
gobject_dep = dependency('gobject-2.0', version : '>= 2.88')
gtk_dep = dependency('gtk+-3.0', version : '>= 3.24.52')

# Prove the bans in prelude.h actually bite. A ban that silently stopped working (prelude
# no longer force-included, macro not #undef'd, ...) fails the configure step here.
probe_args = ['-include', meson.project_source_root() / 'src' / 'prelude.h',
              '-I' + meson.project_build_root(), '-Werror']
probe_deps = [glib_dep, gobject_dep, gtk_dep]
if not cc.compiles('int f(void); int f(void) { return 0; }',
                   args : probe_args, dependencies : probe_deps, name : 'prelude baseline')
  error('prelude.h baseline probe does not compile; ban probes would be meaningless')
endif
banned = {
  'strcpy'           : 'char b[4]; strcpy(b, "x");',
  'sprintf'          : 'char b[4]; sprintf(b, "x");',
  'strncpy'          : 'char b[4]; strncpy(b, "x", 4);',
  'atoi'             : '(void) atoi("1");',
  'system'           : '(void) system("true");',
  'g_error'          : 'g_error("x");',
  'pthread_cancel'   : '(void) pthread_cancel(pthread_self());',
  'gtk_dialog_run'   : '(void) gtk_dialog_run(nullptr);',
  'gtk_main'         : 'gtk_main();',
  'g_main_loop_run'  : 'g_main_loop_run(nullptr);',
  'gtk_clipboard_request_contents' : 'gtk_clipboard_request_contents(nullptr, nullptr, nullptr, nullptr);',
  'gtk_clipboard_wait_for_text' : '(void) gtk_clipboard_wait_for_text(nullptr);',
  'pthread_exit'     : 'pthread_exit(nullptr);',
  'g_thread_exit'    : 'g_thread_exit(nullptr);',
  'alloca'           : '(void) alloca(4);',
  'g_newa'           : '(void) g_newa(char, 4);',
  'g_spawn_command_line_async' : '(void) g_spawn_command_line_async("true", nullptr);',
  'g_signal_connect' : 'g_signal_connect(nullptr, "x", nullptr, nullptr);',
  'g_signal_connect_swapped' : 'g_signal_connect_swapped(nullptr, "x", nullptr, nullptr);',
  'g_signal_connect_object' : 'g_signal_connect_object(nullptr, "x", nullptr, nullptr, 0);',
  'g_idle_add'       : 'g_idle_add(nullptr, nullptr);',
  'g_timeout_add'    : 'g_timeout_add(1, nullptr, nullptr);',
  'g_timeout_add_seconds' : 'g_timeout_add_seconds(1, nullptr, nullptr);',
  'g_source_attach'  : 'g_source_attach(nullptr, nullptr);',
  'gdk_threads_add_idle' : 'gdk_threads_add_idle(nullptr, nullptr);',
  'g_unix_fd_add'    : 'g_unix_fd_add(0, G_IO_IN, nullptr, nullptr);',
  'g_io_add_watch'   : 'g_io_add_watch(nullptr, G_IO_IN, nullptr, nullptr);',
  'g_child_watch_add' : 'g_child_watch_add(0, nullptr, nullptr);',
}
foreach what, code : banned
  if cc.compiles('void f(void); void f(void) { ' + code + ' }',
                 args : probe_args, dependencies : probe_deps, name : 'ban ' + what)
    error('banned API "' + what + '" compiles; prelude.h ban is not working')
  endif
endforeach

subdir('src')
subdir('tests')
