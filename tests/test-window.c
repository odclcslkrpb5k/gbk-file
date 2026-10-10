/* Browser window: navigation, history, hidden toggle, vanished directories (under Xvfb). */
#include "lifetime.h"
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

static bool
no_workers (void *data G_GNUC_UNUSED)
{
  return gbk_scanner_live_workers () == 0;
}

int
main (int argc, char **argv)
{
  gtk_test_init (&argc, &argv, nullptr);
  ctx = gbk_context_new ("/nonexistent/gbk-test.conf");
  app = gtk_application_new ("net.gbk.FileTest", G_APPLICATION_NON_UNIQUE);
  g_autoptr (GError) error = nullptr;
  if (!g_application_register (G_APPLICATION (app), nullptr, &error))
    gbk_fatal ("register: %s", error->message);

  g_test_add_func ("/window/navigation", test_navigation);
  g_test_add_func ("/window/vanishing-directory", test_vanishing_directory);
  g_test_add_func ("/window/terminal-missing", test_terminal_missing);
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
