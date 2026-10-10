#include "launch.h"

bool
gbk_launch_argv (char **argv, const char *cwd, GError **error)
{
  g_assert (argv != nullptr && argv[0] != nullptr);
  /* Without G_SPAWN_DO_NOT_REAP_CHILD GLib reaps the child itself; without
   * G_SPAWN_LEAVE_DESCRIPTORS_OPEN our fds (inotify, eventfds...) are not inherited. */
  GPid pid;
  return g_spawn_async (cwd, argv, nullptr,
                        G_SPAWN_SEARCH_PATH | G_SPAWN_STDIN_FROM_DEV_NULL, nullptr, nullptr,
                        &pid, error);
}

bool
gbk_launch_command (const GbkCommand *cmd, const GbkCommandArgs *args, GError **error)
{
  if (!gbk_command_uses (cmd, 'f') || args->n_files == 0)
    {
      g_auto (GStrv) argv = gbk_command_expand (cmd, args);
      return gbk_launch_argv (argv, args->dir, error);
    }
  bool ok = true;
  for (size_t i = 0; i < args->n_files && ok; i++)
    {
      GbkCommandArgs one = *args;
      one.files = &args->files[i];
      one.n_files = 1;
      char **argv = gbk_command_expand (cmd, &one);
      ok = gbk_launch_argv (argv, args->dir, error);
      g_strfreev (argv);
    }
  return ok;
}
