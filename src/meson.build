deps = [glib_dep, gobject_dep, gtk_dep]

# lifetime.c is the only file allowed to use the raw signal/source APIs (see prelude.h).
lifetime_lib = static_library('gbk-lifetime', 'lifetime.c',
  c_args : '-DGBK_LIFETIME_IMPL',
  include_directories : config_inc,
  dependencies : deps,
)

# Non-GUI core: everything testable without a display.
core_lib = static_library('gbk-core',
  'filetype.c',
  'natsort.c',
  include_directories : config_inc,
  link_with : lifetime_lib,
  dependencies : deps,
)

core_inc = include_directories('.')

executable('gbk-file', 'main.c',
  include_directories : config_inc,
  link_with : [core_lib, lifetime_lib],
  dependencies : deps,
)
