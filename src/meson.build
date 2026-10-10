deps = [glib_dep, gobject_dep, gtk_dep]

# lifetime.c is the only file allowed to use the raw signal/source APIs (see prelude.h).
lifetime_lib = static_library('gbk-lifetime', 'lifetime.c',
  c_args : '-DGBK_LIFETIME_IMPL',
  include_directories : config_inc,
  dependencies : deps,
)

core_inc = include_directories('.')

executable('gbk-file', 'main.c',
  include_directories : config_inc,
  link_with : lifetime_lib,
  dependencies : deps,
)
