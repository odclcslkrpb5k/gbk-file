/* Browser window: navigation, history, hidden toggle, vanished directories (under Xvfb). */
#include "lifetime.h"
#include "job.h"
#include "window.h"

static GtkApplication *app;
static GbkContext *ctx;

typedef bool (*Pred) (void *data);

static bool
wait_until (Pred pred, void *data)
{
  int64_t deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
  while (g_get_monotonic_time () < deadline)
    {
      while (gtk_events_pending ())
        gtk_main_iteration ();
      if (pred (data))
        return true;
      g_usleep (1000);
    }
  return pred (data);
}

static GbkDirModel *
model_of (GbkWindow *w)
{
  return gbk_view_get_model (gbk_window_view (w));
}

static bool
ready (void *data)
{
  GbkDirModel *m = model_of (data);
  return m != nullptr && gbk_dir_model_state (m) == GBK_DIR_READY;
}

typedef struct
{
  GbkWindow *w;
  const char *path;
} PathCtx;

static bool
at_path_and_ready (void *data)
{
  PathCtx *c = data;
  return g_str_equal (gbk_window_path (c->w), c->path) && ready (c->w);
}

static void
wait_at (GbkWindow *w, const char *path)
{
  PathCtx c = { w, path };
  if (!wait_until (at_path_and_ready, &c))
    gbk_fatal ("expected to reach %s, at %s (state %d, error %d, live %d)", path, gbk_window_path (w),
               model_of (w) ? (int) gbk_dir_model_state (model_of (w)) : -1,
               model_of (w) ? gbk_dir_model_error (model_of (w)) : -1,
               model_of (w) ? (int) gbk_dir_model_live (model_of (w)) : -1);
}

static void
mkdirs (const char *root, const char *const *rel)
{
  for (size_t i = 0; rel[i] != nullptr; i++)
    {
      g_autofree char *p = g_build_filename (root, rel[i], nullptr);
      if (g_str_has_suffix (rel[i], "/"))
        g_mkdir_with_parents (p, 0755);
      else
        g_file_set_contents (p, "hello", 5, nullptr);
    }
}

static void
rm_rf (const char *path)
{
  g_autoptr (GDir) d = g_dir_open (path, 0, nullptr);
  const char *name;
  while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
    {
      g_autofree char *child = g_build_filename (path, name, nullptr);
      if (g_file_test (child, G_FILE_TEST_IS_DIR) && !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
        rm_rf (child);
      else
        unlink (child);
    }
  rmdir (path);
}

static const char *
cursor_name (GbkWindow *w)
{
  const GbkEntry *e = gbk_dir_model_get (model_of (w), gbk_view_cursor (gbk_window_view (w)));
  return e != nullptr ? gbk_entry_name (e) : "(none)";
}

static void
activate (GbkWindow *w, const char *action)
{
  g_action_group_activate_action (G_ACTION_GROUP (w), action, nullptr);
}

static void
test_navigation (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "a/", "b/", "sub/", "sub/x", "sub/y", ".hid", "f.txt",
                                        nullptr });
  g_autofree char *sub = g_build_filename (root, "sub", nullptr);

  GbkWindow *w = gbk_window_new (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  g_assert_nonnull (strstr (gbk_window_status (w), "3 folders, 1 file"));
  g_assert_nonnull (strstr (gbk_window_status (w), "Free space:"));

  /* Enter "sub" by activating it, like a double-click. */
  GbkView *v = gbk_window_view (w);
  gbk_view_select_only (v, gbk_dir_model_find (model_of (w), "sub"));
  g_assert_nonnull (strstr (gbk_window_status (w), "\"sub\" folder"));
  g_signal_emit_by_name (v, "activate");
  wait_at (w, sub);
  g_assert_nonnull (strstr (gbk_window_status (w), "2 files"));

  /* Up: back in root with "sub" focused. */
  activate (w, "up");
  wait_at (w, root);
  g_assert_cmpstr (cursor_name (w), ==, "sub");

  activate (w, "back");
  wait_at (w, sub);
  activate (w, "forward");
  wait_at (w, root);
  activate (w, "forward");                      /* nothing forward: no-op */
  wait_at (w, root);

  /* Ctrl+H */
  size_t before = gbk_dir_model_n_visible (model_of (w));
  activate (w, "toggle-hidden");
  g_assert_cmpuint (gbk_dir_model_n_visible (model_of (w)), ==, before + 1);
  activate (w, "toggle-hidden");
  g_assert_cmpuint (gbk_dir_model_n_visible (model_of (w)), ==, before);

  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

static void
test_vanishing_directory (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "keep/", "doomed/", "doomed/deeper/", "doomed/deeper/f",
                                        nullptr });
  g_autofree char *deeper = g_build_filename (root, "doomed", "deeper", nullptr);
  g_autofree char *doomed = g_build_filename (root, "doomed", nullptr);

  GbkWindow *w = gbk_window_new (app, ctx, deeper);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, deeper);

  /* The directory we're in, and its parent, disappear: walk up to the nearest survivor. */
  rm_rf (doomed);
  g_assert_false (g_file_test (doomed, G_FILE_TEST_EXISTS));
  wait_at (w, root);

  /* Opening something that doesn't exist also lands on the nearest ancestor. */
  g_autofree char *nowhere = g_build_filename (root, "no", "such", "dir", nullptr);
  gbk_window_open (w, nowhere);
  wait_at (w, root);

  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

static void
test_terminal_missing (void)
{
  /* The default terminal isn't installed in the test image: F4 must report, not crash. */
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  GbkWindow *w = gbk_window_new (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  activate (w, "terminal");
  g_assert_true (g_str_has_prefix (gbk_window_status (w), "Terminal:"));
  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

/* ---- context menu ---- */

static GtkMenu *
open_context_menu (GbkWindow *w)
{
  /* A real Menu key press, dispatched the way the main loop does it. */
  GbkView *v = gbk_window_view (w);
  gtk_widget_grab_focus (GTK_WIDGET (v));
  GdkEvent *ev = gdk_event_new (GDK_KEY_PRESS);
  ev->key.window = g_object_ref (gtk_widget_get_window (GTK_WIDGET (w)));
  ev->key.keyval = GDK_KEY_Menu;
  ev->key.time = GDK_CURRENT_TIME;
  gdk_event_set_device (ev, gdk_seat_get_keyboard (
      gdk_display_get_default_seat (gtk_widget_get_display (GTK_WIDGET (w)))));
  gtk_main_do_event (ev);
  gdk_event_free (ev);
  GList *menus = gtk_menu_get_for_attach_widget (GTK_WIDGET (v));
  if (g_list_length (menus) != 1)
    gbk_fatal ("expected one context menu, got %u", g_list_length (menus));
  return GTK_MENU (menus->data);
}

static char *
menu_labels (GtkMenu *menu)
{
  GString *s = g_string_new ("");
  g_autoptr (GList) children = gtk_container_get_children (GTK_CONTAINER (menu));
  for (GList *l = children; l != nullptr; l = l->next)
    {
      if (s->len > 0)
        g_string_append_c (s, '|');
      const char *label = GTK_IS_SEPARATOR_MENU_ITEM (l->data)
                            ? "-" : gtk_menu_item_get_label (GTK_MENU_ITEM (l->data));
      g_string_append (s, label);
    }
  return g_string_free (s, false);
}

static void
choose (GtkMenu *menu, const char *label)
{
  g_autoptr (GList) children = gtk_container_get_children (GTK_CONTAINER (menu));
  for (GList *l = children; l != nullptr; l = l->next)
    if (!GTK_IS_SEPARATOR_MENU_ITEM (l->data)
        && g_strcmp0 (gtk_menu_item_get_label (GTK_MENU_ITEM (l->data)), label) == 0)
      {
        gtk_menu_item_activate (GTK_MENU_ITEM (l->data));
        gtk_menu_shell_deactivate (GTK_MENU_SHELL (menu));
        while (gtk_events_pending ())
          gtk_main_iteration ();
        return;
      }
  gbk_fatal ("no menu item \"%s\"", label);
}

static bool
menus_gone (void *data)
{
  return gtk_menu_get_for_attach_widget (GTK_WIDGET (gbk_window_view (data))) == nullptr;
}

static bool
file_exists (void *data)
{
  return g_file_test (data, G_FILE_TEST_EXISTS);
}

static void
test_context_menu (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  g_autofree char *marker = g_build_filename (root, "ran", nullptr);
  g_autofree char *script = g_strdup_printf ("#!/bin/sh\ntouch '%s'\n", marker);
  g_autofree char *prog = g_build_filename (root, "prog", nullptr);
  g_assert_true (g_file_set_contents (prog, script, -1, nullptr));
  chmod (prog, 0755);
  /* "old" is older than "new": sorting by modified must flip them. */
  mkdirs (root, (const char *const[]) { "a-old", "b-new", nullptr });
  g_autofree char *old = g_build_filename (root, "a-old", nullptr);
  struct timespec ts[2] = { { .tv_sec = 1000 }, { .tv_sec = 1000 } };
  utimensat (AT_FDCWD, old, ts, 0);

  GbkWindow *w = gbk_window_new (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  GbkDirModel *m = model_of (w);

  /* Background menu: the directory itself, plus view options. */
  GtkMenu *menu = open_context_menu (w);
  g_autofree char *labels = menu_labels (menu);
  g_assert_cmpstr (labels, ==,
                   "Paste|-|Open Terminal Here|-|Sort by Name|Sort by Date Modified|-|"
                   "Show Hidden Files");
  const char *first = gbk_entry_name (gbk_dir_model_get (m, gbk_dir_model_visible_handle (m, 0)));
  g_assert_cmpstr (first, ==, "a-old");
  choose (menu, "Sort by Date Modified");
  g_assert_cmpint (gbk_dir_model_sort (m), ==, GBK_SORT_MTIME);
  g_assert_cmpstr (gbk_entry_name (gbk_dir_model_get (m, gbk_dir_model_visible_handle (m, 2))),
                   ==, "a-old");                     /* oldest last */
  g_assert_true (wait_until (menus_gone, w));       /* destroyed after closing */

  /* Item menu on an extensionless executable: Execute runs it. */
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (m, "prog"));
  menu = open_context_menu (w);
  g_autofree char *labels2 = menu_labels (menu);
  g_assert_nonnull (strstr (labels2, "Open|"));
  g_assert_nonnull (strstr (labels2, "Execute|Execute in Terminal"));
  choose (menu, "Execute");
  g_assert_true (wait_until (file_exists, marker));
  g_assert_true (wait_until (menus_gone, w));

  /* Double-click (activate) on it runs it too. */
  unlink (marker);
  g_signal_emit_by_name (gbk_window_view (w), "activate");
  g_assert_true (wait_until (file_exists, marker));

  /* Close the window with a menu still open: no callbacks into a dead window. */
  menu = open_context_menu (w);
  gtk_widget_destroy (GTK_WIDGET (w));
  while (gtk_events_pending ())
    gtk_main_iteration ();
  rm_rf (root);
}

/* ---- file operations through the window ---- */

static bool
no_jobs (void *data G_GNUC_UNUSED)
{
  return gbk_job_live () == 0;
}

static bool
path_exists (void *data)
{
  return g_file_test (data, G_FILE_TEST_EXISTS);
}

static bool
path_gone (void *data)
{
  return !g_file_test (data, G_FILE_TEST_EXISTS);
}

typedef struct
{
  GbkWindow *w;
  const char *name;
} SelCtx;

static bool
only_selected (void *data)
{
  SelCtx *c = data;
  GbkView *v = gbk_window_view (c->w);
  GbkHandle h = gbk_dir_model_find (model_of (c->w), c->name);
  return h != GBK_HANDLE_NONE && gbk_view_n_selected (v) == 1 && gbk_view_is_selected (v, h);
}

/* The topmost visible dialog other than our windows (async dialogs: find, then respond). */
static GtkDialog *
find_dialog (void)
{
  for (int tries = 0; tries < 5000; tries++)
    {
      g_autoptr (GList) tops = gtk_window_list_toplevels ();
      for (GList *l = tops; l != nullptr; l = l->next)
        if (GTK_IS_DIALOG (l->data) && gtk_widget_get_visible (l->data))
          return l->data;
      while (gtk_events_pending ())
        gtk_main_iteration ();
      g_usleep (1000);
    }
  gbk_fatal ("no dialog appeared");
}

static void
select_name (GbkWindow *w, const char *name)
{
  GbkHandle h = gbk_dir_model_find (model_of (w), name);
  if (h == GBK_HANDLE_NONE)
    gbk_fatal ("no %s", name);
  gbk_view_select_only (gbk_window_view (w), h);
}

static void
test_file_operations (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "src/", "dst/", "src/a.txt", "src/b.txt",
                                        "dst/b.txt", nullptr });
  g_autofree char *src = g_build_filename (root, "src", nullptr);
  g_autofree char *dst = g_build_filename (root, "dst", nullptr);
  GbkWindow *w = gbk_window_new (app, ctx, src);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, src);

  /* Copy a.txt, go to dst, paste: it's copied and selected there. */
  select_name (w, "a.txt");
  activate (w, "copy");
  gbk_window_open (w, dst);
  wait_at (w, dst);
  activate (w, "paste");
  g_autofree char *dst_a = g_build_filename (dst, "a.txt", nullptr);
  g_assert_true (wait_until (path_exists, dst_a));
  SelCtx sel_a = { w, "a.txt" };
  g_assert_true (wait_until (only_selected, &sel_a));
  g_assert_true (wait_until (no_jobs, nullptr));

  /* Cut b.txt from src, paste into dst where b.txt exists: answer Keep Both. */
  activate (w, "back");
  wait_at (w, src);
  select_name (w, "b.txt");
  activate (w, "cut");
  gbk_window_open (w, dst);
  wait_at (w, dst);
  activate (w, "paste");
  GtkDialog *conflict = find_dialog ();
  gtk_dialog_response (conflict, 3 /* RESP_KEEP_BOTH */);
  g_autofree char *kept = g_build_filename (dst, "b (1).txt", nullptr);
  g_autofree char *src_b = g_build_filename (src, "b.txt", nullptr);
  g_assert_true (wait_until (path_exists, kept));
  g_assert_true (wait_until (path_gone, src_b));      /* it was a cut: moved */
  g_assert_true (wait_until (no_jobs, nullptr));

  /* F2 rename via the dialog. */
  select_name (w, "a.txt");
  activate (w, "rename");
  GtkDialog *rename = find_dialog ();
  GtkEntry *entry = g_object_get_data (G_OBJECT (rename), "gbk-entry");
  gtk_entry_set_text (entry, "renamed.txt");
  gtk_dialog_response (rename, GTK_RESPONSE_ACCEPT);
  g_autofree char *renamed = g_build_filename (dst, "renamed.txt", nullptr);
  g_assert_true (wait_until (path_exists, renamed));
  SelCtx sel_r = { w, "renamed.txt" };
  g_assert_true (wait_until (only_selected, &sel_r));

  /* Delete key: to the trash (no confirmation). */
  g_assert_true (wait_until (no_jobs, nullptr));
  activate (w, "trash");
  g_assert_true (wait_until (path_gone, renamed));
  g_assert_true (wait_until (no_jobs, nullptr));

  /* Shift+Delete: confirmation; Cancel keeps the file, Delete removes it. */
  select_name (w, "b.txt");
  activate (w, "delete");
  gtk_dialog_response (find_dialog (), GTK_RESPONSE_CANCEL);
  g_autofree char *dst_b = g_build_filename (dst, "b.txt", nullptr);
  g_assert_true (g_file_test (dst_b, G_FILE_TEST_EXISTS));
  activate (w, "delete");
  gtk_dialog_response (find_dialog (), GTK_RESPONSE_ACCEPT);
  g_assert_true (wait_until (path_gone, dst_b));
  g_assert_true (wait_until (no_jobs, nullptr));

  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

static void
test_job_outlives_window (void)
{
  /* Close the window while its paste is still asking a question: the job carries on with
   * its own UI, and cancelling it there cleans up. Nothing may call into the dead window. */
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "f.txt", nullptr });
  GbkWindow *w = gbk_window_new (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  select_name (w, "f.txt");
  activate (w, "copy");
  /* Paste where it already is: a copy... but pasting twice makes a conflict-free copy, so
   * create the conflict explicitly by pasting into a folder holding f.txt. */
  mkdirs (root, (const char *const[]) { "sub/", "sub/f.txt", nullptr });
  g_autofree char *sub = g_build_filename (root, "sub", nullptr);
  gbk_window_open (w, sub);
  wait_at (w, sub);
  activate (w, "paste");
  GtkDialog *conflict = find_dialog ();
  gtk_widget_destroy (GTK_WIDGET (w));
  while (gtk_events_pending ())
    gtk_main_iteration ();
  gtk_dialog_response (conflict, GTK_RESPONSE_CANCEL);
  g_assert_true (wait_until (no_jobs, nullptr));
  rm_rf (root);
}

static bool
no_workers (void *data G_GNUC_UNUSED)
{
  return gbk_scanner_live_workers () == 0;
}

int
main (int argc, char **argv)
{
  /* Private data dir: the trash goes here, never into anyone's real one. */
  g_autofree char *data_home = g_dir_make_tmp ("gbk-win-data-XXXXXX", nullptr);
  g_setenv ("XDG_DATA_HOME", data_home, true);
  gtk_test_init (&argc, &argv, nullptr);
  ctx = gbk_context_new ("/nonexistent/gbk-test.conf");
  app = gtk_application_new ("net.gbk.FileTest", G_APPLICATION_NON_UNIQUE);
  g_autoptr (GError) error = nullptr;
  if (!g_application_register (G_APPLICATION (app), nullptr, &error))
    gbk_fatal ("register: %s", error->message);

  g_test_add_func ("/window/navigation", test_navigation);
  g_test_add_func ("/window/vanishing-directory", test_vanishing_directory);
  g_test_add_func ("/window/terminal-missing", test_terminal_missing);
  g_test_add_func ("/window/context-menu", test_context_menu);
  g_test_add_func ("/window/file-operations", test_file_operations);
  g_test_add_func ("/window/job-outlives-window", test_job_outlives_window);
  int status = g_test_run ();

  while (gtk_events_pending ())
    gtk_main_iteration ();
  g_object_unref (app);
  gbk_context_unref (ctx);
  wait_until (no_workers, nullptr);
  if (gbk_lifetime_live () != 0)
    gbk_fatal ("%d registrations leaked", gbk_lifetime_live ());
  return status;
}
