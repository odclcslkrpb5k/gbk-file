/* Launching for real: scripts and a fake default application write marker files. */
#include "open.h"

static char *tmp;
static GbkConfig *config;

static char *
path_in (const char *name)
{
  return g_build_filename (tmp, name, nullptr);
}

static void
write_file (const char *name, const char *content, int mode)
{
  g_autofree char *p = path_in (name);
  g_assert_true (g_file_set_contents (p, content, -1, nullptr));
  g_assert_cmpint (chmod (p, (mode_t) mode), ==, 0);
}

static bool
wait_for (const char *path)
{
  for (int i = 0; i < 5000; i++)
    {
      if (g_file_test (path, G_FILE_TEST_EXISTS))
        return true;
      g_usleep (1000);
    }
  return false;
}

static GbkItem *
item (const char *name, const char *mime, bool exec, bool script)
{
  GbkItem *it = g_new0 (GbkItem, 1);
  it->path = path_in (name);
  it->display = g_strdup (name);
  it->mime = g_strdup (mime);
  it->executable = exec;
  it->script = script;
  return it;
}

static void
test_plan (void)
{
  g_autoptr (GbkItem) dir = gbk_item_new_dir ("/x");
  g_autoptr (GbkItem) script = item ("s.sh", "application/x-shellscript", true, true);
  g_autoptr (GbkItem) cold = item ("s.sh", "application/x-shellscript", false, true);
  g_autoptr (GbkItem) bin = item ("prog", "application/x-executable", true, false);
  g_autoptr (GbkItem) txt = item ("t.txt", "text/plain", false, false);
  g_assert_cmpint (gbk_open_plan (dir), ==, GBK_OPEN_ENTER);
  g_assert_cmpint (gbk_open_plan (script), ==, GBK_OPEN_RUN_TERMINAL);
  g_assert_cmpint (gbk_open_plan (cold), ==, GBK_OPEN_DEFAULT);   /* non-exec script: editor */
  g_assert_cmpint (gbk_open_plan (bin), ==, GBK_OPEN_RUN);
  g_assert_cmpint (gbk_open_plan (txt), ==, GBK_OPEN_DEFAULT);

  g_auto (GStrv) a1 = gbk_open_exec_argv (config, script);
  g_assert_cmpuint (g_strv_length (a1), ==, 1);
  g_auto (GStrv) a2 = gbk_open_exec_argv (config, cold);
  g_assert_cmpstr (a2[0], ==, "sh");
  g_assert_cmpstr (a2[1], ==, script->path);
  g_assert_null (gbk_open_exec_argv (config, txt));
  g_assert_null (gbk_open_exec_argv (config, dir));
}

static void
test_execute (void)
{
  g_autofree char *m1 = path_in ("ran-direct");
  g_autofree char *m2 = path_in ("ran-terminal");
  g_autofree char *m3 = path_in ("ran-interp");
  g_autofree char *s1 = g_strdup_printf ("#!/bin/sh\ntouch '%s'\n", m1);
  g_autofree char *s2 = g_strdup_printf ("#!/bin/sh\ntouch '%s'\n", m2);
  g_autofree char *s3 = g_strdup_printf ("touch '%s'\n", m3);
  write_file ("direct", s1, 0755);
  write_file ("term.sh", s2, 0755);
  write_file ("cold.sh", s3, 0644);

  g_autoptr (GbkItem) direct = item ("direct", "application/x-executable", true, false);
  g_autoptr (GbkItem) term = item ("term.sh", "application/x-shellscript", true, true);
  g_autoptr (GbkItem) cold = item ("cold.sh", "application/x-shellscript", false, true);
  g_autoptr (GError) error = nullptr;
  g_assert_true (gbk_open_execute (config, direct, false, tmp, &error));
  g_assert_true (gbk_open_execute (config, term, true, tmp, &error));
  g_assert_true (gbk_open_execute (config, cold, false, tmp, &error));
  g_assert_no_error (error);
  g_assert_true (wait_for (m1));
  g_assert_true (wait_for (m2));
  g_assert_true (wait_for (m3));
}

static void
test_default_app (void)
{
  /* Two text files open in one launch of the fake default app; awkward names intact. */
  write_file ("-rf.txt", "", 0644);
  write_file ("a b.txt", "", 0644);
  g_autoptr (GPtrArray) items = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_item_free);
  g_ptr_array_add (items, item ("-rf.txt", "text/plain", false, false));
  g_ptr_array_add (items, item ("a b.txt", "text/plain", false, false));
  g_autoptr (GError) error = nullptr;
  g_assert_true (gbk_open_default (items, nullptr, &error));
  g_assert_no_error (error);
  g_autofree char *o1 = path_in ("-rf.txt.opened");
  g_autofree char *o2 = path_in ("a b.txt.opened");
  g_assert_true (wait_for (o1));
  g_assert_true (wait_for (o2));

  g_autoptr (GPtrArray) none = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_item_free);
  g_ptr_array_add (none, item ("x.weird", "application/x-gbk-nothing-opens-this", false, false));
  g_autoptr (GError) error2 = nullptr;
  g_assert_false (gbk_open_default (none, nullptr, &error2));
  g_assert_error (error2, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

static void
test_action_and_missing_program (void)
{
  const GbkAction *a = g_ptr_array_index (config->actions, 0);
  g_autoptr (GPtrArray) items = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_item_free);
  g_ptr_array_add (items, item ("act1", "text/plain", false, false));
  g_ptr_array_add (items, item ("act2", "text/plain", false, false));
  g_autoptr (GError) error = nullptr;
  g_assert_true (gbk_open_action (a, items, tmp, &error));
  g_autofree char *t1 = path_in ("act1");
  g_autofree char *t2 = path_in ("act2");
  g_assert_true (wait_for (t1));
  g_assert_true (wait_for (t2));

  /* The configured terminal doesn't exist here: an error, not a crash. */
  g_autoptr (GbkConfig) bad = gbk_config_new_default ();
  g_autoptr (GError) error2 = nullptr;
  g_assert_false (gbk_open_terminal (bad, tmp, &error2));
  g_assert_nonnull (error2);
}

int
main (int argc, char **argv)
{
  tmp = g_dir_make_tmp ("gbk-open-XXXXXX", nullptr);

  /* A private XDG environment with a fake default app for text/plain. Must be set before
   * GIO first reads the application database. */
  g_autofree char *data = path_in ("xdg-data");
  g_autofree char *conf_home = path_in ("xdg-config");
  g_autofree char *apps = g_build_filename (data, "applications", nullptr);
  g_mkdir_with_parents (apps, 0755);
  g_mkdir_with_parents (conf_home, 0755);
  g_autofree char *opener = path_in ("opener");
  {
    g_autofree char *script = g_strdup ("#!/bin/sh\nfor f; do touch \"$f.opened\"; done\n");
    g_assert_true (g_file_set_contents (opener, script, -1, nullptr));
    chmod (opener, 0755);
  }
  g_autofree char *desktop = g_strdup_printf (
      "[Desktop Entry]\nType=Application\nName=Test Opener\nExec=%s %%F\nMimeType=text/plain;\n",
      opener);
  g_autofree char *desktop_path = g_build_filename (apps, "gbk-test-opener.desktop", nullptr);
  g_assert_true (g_file_set_contents (desktop_path, desktop, -1, nullptr));
  g_autofree char *mimeapps = g_build_filename (conf_home, "mimeapps.list", nullptr);
  g_assert_true (g_file_set_contents (
      mimeapps, "[Default Applications]\ntext/plain=gbk-test-opener.desktop\n", -1, nullptr));
  g_setenv ("XDG_DATA_HOME", data, true);
  g_setenv ("XDG_CONFIG_HOME", conf_home, true);

  g_test_init (&argc, &argv, nullptr);
  const char conf[] =
    "[General]\nTerminalRun=env %c\n"
    "[Action touch]\nName=Touch\nTypes=*\nCommand=touch %F\n";
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func (g_free);
  config = gbk_config_load_data (conf, strlen (conf), errors);
  g_assert_cmpuint (errors->len, ==, 0);

  g_test_add_func ("/open/plan", test_plan);
  g_test_add_func ("/open/execute", test_execute);
  g_test_add_func ("/open/default-app", test_default_app);
  g_test_add_func ("/open/action-and-missing-program", test_action_and_missing_program);
  int status = g_test_run ();

  gbk_config_free (config);
  g_auto (GStrv) rm = g_new0 (char *, 4);
  rm[0] = g_strdup ("rm");
  rm[1] = g_strdup ("-rf");
  rm[2] = g_strdup (tmp);
  g_spawn_sync (nullptr, rm, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr);
  g_free (tmp);
  return status;
}
