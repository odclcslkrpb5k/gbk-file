#include "config.h"

static GbkConfig *
load (const char *text, GPtrArray *errors)
{
  return gbk_config_load_data (text, strlen (text), errors);
}

static void
assert_errors (const char *text, const char *expected_fragment)
{
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GbkConfig) c = load (text, errors);
  g_assert_cmpuint (errors->len, >, 0);
  bool found = false;
  for (guint i = 0; i < errors->len; i++)
    found |= strstr (g_ptr_array_index (errors, i), expected_fragment) != nullptr;
  if (!found)
    {
      for (guint i = 0; i < errors->len; i++)
        g_test_message ("error: %s", (char *) g_ptr_array_index (errors, i));
      g_assert_true (found);
    }
  /* rejected: defaults in force */
  g_assert_cmpstr (gbk_command_template (c->terminal), ==, "xfce4-terminal --working-directory=%d");
  g_assert_cmpuint (c->actions->len, ==, 0);
}

static void
test_defaults (void)
{
  g_autoptr (GbkConfig) c = gbk_config_new_default ();
  g_assert_false (c->show_hidden);
  g_assert_cmpint (c->sort, ==, GBK_SORT_NAME);
  g_assert_true (gbk_command_uses (c->terminal_run, 'c'));
  g_assert_cmpstr (gbk_config_interpreter (c, "x.py")[0], ==, "python3");
  g_assert_cmpstr (gbk_config_interpreter (c, "X.SH")[0], ==, "sh");
  g_assert_null (gbk_config_interpreter (c, "x.txt"));
  g_assert_null (gbk_config_interpreter (c, ".sh"));     /* dotfile, no extension */
  g_assert_null (gbk_config_interpreter (c, "sh"));
}

static void
test_example_conf (void)
{
  /* docs/example.conf must always load cleanly: the documentation can't rot. */
  g_autofree char *path = g_build_filename (g_getenv ("GBK_SOURCE_ROOT"), "docs", "example.conf",
                                            nullptr);
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GbkConfig) c = gbk_config_load_file (path, errors);
  for (guint i = 0; i < errors->len; i++)
    g_test_message ("error: %s", (char *) g_ptr_array_index (errors, i));
  g_assert_cmpuint (errors->len, ==, 0);
  g_assert_cmpuint (c->actions->len, ==, 3);
  const GbkAction *a = g_ptr_array_index (c->actions, 0);
  g_assert_cmpstr (a->id, ==, "extract-here");
  g_assert_true (gbk_action_matches (a, "application/zip", false));
  g_assert_false (gbk_action_matches (a, "text/plain", false));
  g_assert_false (gbk_action_matches (a, "inode/directory", true));
  const GbkAction *compress = g_ptr_array_index (c->actions, 2);
  g_assert_true (gbk_action_matches (compress, "text/plain", false));
  g_assert_true (gbk_action_matches (compress, "inode/directory", true));
}

static void
test_full (void)
{
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GbkConfig) c = load (
      "[General]\n"
      "Terminal=st -d %d\n"
      "TerminalRun=st -d %d -e %c\n"
      "ShowHidden=true\n"
      "SortBy=modified\n"
      "[Interpreters]\n"
      "py=python3 -u\n"
      "[Types]\n"
      "foo=application/x-foo\n"
      "[Action view]\n"
      "Name=View\n"
      "Types=image/*\n"
      "Command=viewer %f\n",
      errors);
  g_assert_cmpuint (errors->len, ==, 0);
  g_assert_true (c->show_hidden);
  g_assert_cmpint (c->sort, ==, GBK_SORT_MTIME);
  g_assert_cmpstr (gbk_command_template (c->terminal), ==, "st -d %d");
  const char *const *py = gbk_config_interpreter (c, "a.py");
  g_assert_cmpstr (py[0], ==, "python3");
  g_assert_cmpstr (py[1], ==, "-u");
  g_assert_null (gbk_config_interpreter (c, "a.sh"));   /* section replaced defaults */
  const GbkAction *view = g_ptr_array_index (c->actions, 0);
  g_assert_true (gbk_action_matches (view, "image/png", false));
  g_assert_false (gbk_action_matches (view, "imagex/png", false));
  g_assert_false (gbk_action_matches (view, "video/mp4", false));

  GbkTypeBuilder *b = gbk_type_builder_new ();
  gbk_config_apply_types (c, b);
  g_autoptr (GbkTypeTable) t = gbk_type_builder_finish (b);
  g_assert_cmpstr (gbk_type_table_mime (t, gbk_type_table_lookup (t, "x.foo", 5,
                                                                   GBK_KIND_REGULAR, false)),
                   ==, "application/x-foo");
}

static void
test_errors (void)
{
  assert_errors ("[Genral]\n", "unknown section [Genral]");
  assert_errors ("[General]\nTerminl=x\n", "unknown key \"Terminl\"");
  assert_errors ("[General]\nShowHidden=yes\n", "expected true or false");
  assert_errors ("[General]\nSortBy=size\n", "expected name or modified");
  assert_errors ("[General]\nTerminal=st %f\n", "not allowed here");
  assert_errors ("[General]\nTerminalRun=st -d %d\n", "must contain %c");
  assert_errors ("[General]\nTerminal=st 'oops\n", "can't parse command");
  assert_errors ("[Interpreters]\n.py=python3\n", "not a valid extension");
  assert_errors ("[Interpreters]\npy=python3 %f\n", "no placeholders");
  assert_errors ("[Types]\nfoo=notamime\n", "not a valid extension and MIME type");
  assert_errors ("[Action ]\nName=x\nCommand=x\nTypes=*\n", "needs an id");
  assert_errors ("[Action a]\nName=x\nTypes=*\n", "Command");
  assert_errors ("[Action a]\nCommand=x\nTypes=*\n", "Name");
  assert_errors ("[Action a]\nName=x\nCommand=x\n", "matches nothing");
  assert_errors ("[Action a]\nName=x\nCommand=x\nTypes=image/png*\n", "type/* pattern");
  assert_errors ("[Action a]\nName=x\nCommand=x %c\nTypes=*\n", "not allowed here");
  assert_errors ("not a key file", "");
  /* A bad entry anywhere rejects everything, including valid parts. */
  assert_errors ("[General]\nShowHidden=true\n[Bogus]\n", "unknown section");
}

static void
test_missing_file (void)
{
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GbkConfig) c = gbk_config_load_file ("/nonexistent/gbk.conf", errors);
  g_assert_cmpuint (errors->len, ==, 0);
  g_assert_nonnull (c->terminal);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  g_test_add_func ("/config/defaults", test_defaults);
  g_test_add_func ("/config/example-conf", test_example_conf);
  g_test_add_func ("/config/full", test_full);
  g_test_add_func ("/config/errors", test_errors);
  g_test_add_func ("/config/missing-file", test_missing_file);
  return g_test_run ();
}
