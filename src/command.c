#include "command.h"

G_DEFINE_QUARK (gbk-command-error-quark, gbk_command_error)

struct GbkCommand
{
  char *tmpl;
  char **argv;          /* tokens, placeholders still in them */
  char used[8];         /* placeholder letters present, NUL-terminated */
};

static void
mark_used (GbkCommand *c, char p)
{
  if (strchr (c->used, p) != nullptr)
    return;
  size_t n = strlen (c->used);
  g_assert (n + 1 < sizeof c->used);
  c->used[n] = p;
}

static bool
check_token (GbkCommand *c, const char *tok, bool is_program, const char *allowed, GError **error)
{
  for (const char *p = tok; *p != '\0'; p++)
    {
      if (*p != '%')
        continue;
      char ph = p[1];
      if (ph == '%')
        {
          p++;
          continue;
        }
      if (ph == '\0' || strchr ("dfFc", ph) == nullptr)
        {
          g_set_error (error, GBK_COMMAND_ERROR, 0, "unknown placeholder \"%%%c\"",
                       ph == '\0' ? ' ' : ph);
          return false;
        }
      if (strchr (allowed, ph) == nullptr)
        {
          g_set_error (error, GBK_COMMAND_ERROR, 0, "placeholder %%%c is not allowed here", ph);
          return false;
        }
      if (is_program)
        {
          g_set_error (error, GBK_COMMAND_ERROR, 0, "the program name can't be a placeholder");
          return false;
        }
      if ((ph == 'F' || ph == 'c') && !g_str_equal (tok, ph == 'F' ? "%F" : "%c"))
        {
          g_set_error (error, GBK_COMMAND_ERROR, 0, "%%%c must be a whole argument", ph);
          return false;
        }
      mark_used (c, ph);
      p++;
    }
  return true;
}

GbkCommand *
gbk_command_parse (const char *tmpl, const char *allowed, GError **error)
{
  g_autoptr (GbkCommand) c = g_new0 (GbkCommand, 1);
  c->tmpl = g_strdup (tmpl);

  int argc = 0;
  g_autoptr (GError) shell_error = nullptr;
  if (!g_shell_parse_argv (tmpl, &argc, &c->argv, &shell_error))
    {
      g_set_error (error, GBK_COMMAND_ERROR, 0, "can't parse command: %s", shell_error->message);
      return nullptr;
    }
  if (c->argv[0][0] == '\0')
    {
      g_set_error (error, GBK_COMMAND_ERROR, 0, "empty program name");
      return nullptr;
    }
  for (int i = 0; i < argc; i++)
    if (!check_token (c, c->argv[i], i == 0, allowed, error))
      return nullptr;
  if (gbk_command_uses (c, 'f') && gbk_command_uses (c, 'F'))
    {
      g_set_error (error, GBK_COMMAND_ERROR, 0, "use %%f or %%F, not both");
      return nullptr;
    }
  return g_steal_pointer (&c);
}

void
gbk_command_free (GbkCommand *c)
{
  if (c == nullptr)
    return;
  g_strfreev (c->argv);
  g_free (c->tmpl);
  g_free (c);
}

const char *
gbk_command_template (const GbkCommand *c)
{
  return c->tmpl;
}

bool
gbk_command_uses (const GbkCommand *c, char placeholder)
{
  return placeholder != '\0' && strchr (c->used, placeholder) != nullptr;
}

/* Substitute %d / %f / %% inside one token. */
static char *
expand_token (const char *tok, const GbkCommandArgs *a)
{
  GString *out = g_string_new ("");
  for (const char *p = tok; *p != '\0'; p++)
    {
      if (*p != '%')
        {
          g_string_append_c (out, *p);
          continue;
        }
      p++;
      switch (*p)
        {
        case '%': g_string_append_c (out, '%'); break;
        case 'd': g_string_append (out, a->dir); break;
        case 'f': g_string_append (out, a->files[0]); break;
        default: gbk_fatal ("unvalidated placeholder %%%c", *p);   /* parse rejects these */
        }
    }
  return g_string_free (out, false);
}

char **
gbk_command_expand (const GbkCommand *c, const GbkCommandArgs *a)
{
  g_assert (a->dir != nullptr && a->dir[0] == '/');
  if (gbk_command_uses (c, 'f') && a->n_files != 1)
    gbk_fatal ("%%f command expanded with %zu files", a->n_files);
  for (size_t i = 0; i < a->n_files; i++)
    g_assert (a->files[i] != nullptr && a->files[i][0] == '/');

  GPtrArray *out = g_ptr_array_new ();
  for (size_t i = 0; c->argv[i] != nullptr; i++)
    {
      const char *tok = c->argv[i];
      if (g_str_equal (tok, "%F"))
        for (size_t j = 0; j < a->n_files; j++)
          g_ptr_array_add (out, g_strdup (a->files[j]));
      else if (g_str_equal (tok, "%c"))
        for (size_t j = 0; j < a->n_run; j++)
          g_ptr_array_add (out, g_strdup (a->run[j]));
      else
        g_ptr_array_add (out, expand_token (tok, a));
    }
  g_ptr_array_add (out, nullptr);
  return (char **) g_ptr_array_free (out, false);
}
