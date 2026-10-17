/* Starting external programs. Always argv, never a shell; children are reaped by GLib. */
#pragma once

#include "command.h"

/* Spawn argv with `cwd` as working directory. argv[0] is looked up in PATH. */
[[nodiscard]] bool gbk_launch_argv (char **argv, const char *cwd, GError **error);

/* Expand `cmd` and spawn it once (or once per file for %f commands). */
[[nodiscard]] bool gbk_launch_command (const GbkCommand *cmd, const GbkCommandArgs *args,
                                       GError **error);
