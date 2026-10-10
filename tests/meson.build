test_env = environment()
# Any GLib warning or critical (e.g. a failed g_return_if_fail) aborts the test.
test_env.set('G_DEBUG', 'fatal-warnings,fatal-criticals,gc-friendly')
test_env.set('G_ENABLE_DIAGNOSTIC', '1')
test_env.set('ASAN_OPTIONS', 'detect_leaks=1:abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1')
test_env.set('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')

foreach name : ['lifetime']
  exe = executable('test-' + name, 'test-' + name + '.c',
    include_directories : [config_inc, core_inc],
    link_with : lifetime_lib,
    dependencies : deps,
  )
  test(name, exe, env : test_env, protocol : 'tap', args : ['--tap'])
endforeach
