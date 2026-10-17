#include "command.h"

static char *
run (const char *tmpl, const char *allowed, const char *const *files, size_t n)
{
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkCommand) c = gbk_command_parse (tmpl, allowed, &error);
  g_assert_no_error (error);
  const char *const runv[] = { "/usr/bin/python3", "/home/u/my script.py" };
  GbkCommandArgs a
      = { .dir = "/home/u/dir with space", .files = files, .n_files = n, .run = runv, .n_run = 2 };
  g_auto (GStrv) argv = gbk_command_expand (c, &a);
  return g_strjoinv ("|", argv);
}

static void
assert_rejected (const char *tmpl, const char *allowed)
{
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkCommand) c = gbk_command_parse (tmpl, allowed, &error);
  g_assert_null (c);
  g_assert_error (error, GBK_COMMAND_ERROR, 0);
}

static void
test_expand (void)
{
  const char *const one[] = { "/x/a b.zip" };
  const char *const many[] = { "/x/a", "/x/-rf", "/x/$(reboot)" };
  g_autofree char *r1 = run ("xfce4-terminal --working-directory=%d", "d", nullptr, 0);
  g_assert_cmpstr (r1, ==, "xfce4-terminal|--working-directory=/home/u/dir with space");
  g_autofree char *r2 = run ("file-roller --extract-here %f", "dfF", one, 1);
  g_assert_cmpstr (r2, ==, "file-roller|--extract-here|/x/a b.zip");
  g_autofree char *r3 = run ("tool %F --end", "dfF", many, 3);
  g_assert_cmpstr (r3, ==, "tool|/x/a|/x/-rf|/x/$(reboot)|--end");
  g_autofree char *r4 = run ("term --hold -x %c", "dc", nullptr, 0);
  g_assert_cmpstr (r4, ==, "term|--hold|-x|/usr/bin/python3|/home/u/my script.py");
  g_autofree char *r5 = run ("echo 100%% '%d'", "d", nullptr, 0);
  g_assert_cmpstr (r5, ==, "echo|100%|/home/u/dir with space");
  g_autofree char *r6 = run ("tool %F", "dfF", nullptr, 0);
  g_assert_cmpstr (r6, ==, "tool");
}

static void
test_quoting_is_not_a_shell (void)
{
  /* ; | $() are just characters in arguments: nothing runs them. */
  g_autofree char *r = run ("echo a;b $(x) | y", "d", nullptr, 0);
  g_assert_cmpstr (r, ==, "echo|a;b|$(x)|||y");
}

static void
test_rejects (void)
{
  assert_rejected ("", "d");
  assert_rejected ("\"\" arg", "d");
  assert_rejected ("tool 'unterminated", "d");
  assert_rejected ("tool %x", "dfF");
  assert_rejected ("tool %", "dfF");
  assert_rejected ("tool %f", "d");              /* not allowed in this context */
  assert_rejected ("%d/tool", "d");              /* program must be literal */
  assert_rejected ("tool --files=%F", "dfF");    /* %F must be whole */
  assert_rejected ("tool x%c", "dc");
  assert_rejected ("tool %f %F", "dfF");
}

static void
test_uses (void)
{
  g_autoptr (GbkCommand) c = gbk_command_parse ("t %f --in=%d", "dfF", nullptr);
  g_assert_nonnull (c);
  g_assert_true (gbk_command_uses (c, 'f'));
  g_assert_true (gbk_command_uses (c, 'd'));
  g_assert_false (gbk_command_uses (c, 'F'));
  g_assert_cmpstr (gbk_command_template (c), ==, "t %f --in=%d");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  g_test_add_func ("/command/expand", test_expand);
  g_test_add_func ("/command/not-a-shell", test_quoting_is_not_a_shell);
  g_test_add_func ("/command/rejects", test_rejects);
  g_test_add_func ("/command/uses", test_uses);
  return g_test_run ();
}
