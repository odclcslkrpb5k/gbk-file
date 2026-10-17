/* Command templates from the config, e.g. "xfce4-terminal --working-directory=%d".
 *
 * Parsed once into an argv template (shell-like quoting, but no shell ever runs). Expansion
 * substitutes each placeholder within its own argument, so filenames can never inject
 * arguments or shell syntax. File placeholders expand to absolute paths, so a file named
 * "-rf" arrives as "/dir/-rf" and can't be mistaken for an option.
 *
 * Placeholders:
 *   %d  the current directory (may be embedded: --working-directory=%d)
 *   %f  one file: the command runs once per selected file (may be embedded)
 *   %F  all selected files, one argument each (must be a whole argument)
 *   %c  a command to run: the file, or interpreter + file (must be a whole argument)
 *   %%  a literal %
 * The program (first argument) must be literal. */
#pragma once

typedef struct GbkCommand GbkCommand;

#define GBK_COMMAND_ERROR (gbk_command_error_quark ())
GQuark gbk_command_error_quark (void);

/* `allowed` lists the placeholder letters valid in this context, e.g. "d" or "dfF". */
[[nodiscard]] GbkCommand *gbk_command_parse (const char *tmpl, const char *allowed, GError **error);
void gbk_command_free (GbkCommand *c);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkCommand, gbk_command_free)

const char *gbk_command_template (const GbkCommand *c);
bool gbk_command_uses (const GbkCommand *c, char placeholder);

typedef struct
{
  const char *dir;              /* absolute */
  const char *const *files;     /* absolute paths */
  size_t n_files;
  const char *const *run;       /* argv for %c */
  size_t n_run;
} GbkCommandArgs;

/* Returns a NULL-terminated argv (g_strfreev). For %f, n_files must be 1: callers run the
 * command once per file. */
[[nodiscard]] char **gbk_command_expand (const GbkCommand *c, const GbkCommandArgs *args);
