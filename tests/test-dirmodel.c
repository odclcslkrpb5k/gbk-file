/* Directory model logic, driven by synthetic scanner messages (no filesystem). */
#include "dirmodel.h"
#include "lifetime.h"

static GbkTypeTable *types;

static GbkEntry *
mk (const char *name, GbkFileKind kind, int64_t mtime)
{
  /* Size follows mtime, so the size sort has something to order by too. */
  GbkEntryInfo info = { .kind = kind, .mtime_ns = mtime, .size = (uint64_t) MAX (mtime, 0),
                        .mode = 0644 };
  GbkEntry *e = gbk_entry_new (name, strlen (name), &info, types);
  g_assert_nonnull (e);
  return e;
}

static GbkScanMsg *
msg (GbkScanMsgType type)
{
  GbkScanMsg *m = g_new0 (GbkScanMsg, 1);
  m->type = type;
  m->upserts = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_entry_free);
  m->removes = g_ptr_array_new_with_free_func (g_free);
  return m;
}

/* One-chunk snapshot of files (and dirs, names ending in '/'). */
#define SNAPSHOT(m, ...) \
  snapshot (m, (const char *const[]) { __VA_ARGS__ }, \
            G_N_ELEMENTS (((const char *const[]) { __VA_ARGS__ })))

static void
snapshot (GbkDirModel *m, const char *const *names, size_t n)
{
  GbkScanMsg *s = msg (GBK_SCAN_CHUNK);
  s->first = s->last = true;
  s->live = true;
  for (size_t i = 0; i < n; i++)
    {
      size_t len = strlen (names[i]);
      if (len > 0 && names[i][len - 1] == '/')
        {
          g_autofree char *dir = g_strndup (names[i], len - 1);
          g_ptr_array_add (s->upserts, mk (dir, GBK_KIND_DIRECTORY, 0));
        }
      else
        g_ptr_array_add (s->upserts, mk (names[i], GBK_KIND_REGULAR, (int64_t) i));
    }
  gbk_dir_model_apply_for_test (m, s);
  gbk_dir_model_check_order (m);
}

static char *
visible_names (GbkDirModel *m)
{
  GString *out = g_string_new ("");
  for (size_t i = 0; i < gbk_dir_model_n_visible (m); i++)
    {
      const GbkEntry *e = gbk_dir_model_get (m, gbk_dir_model_visible_handle (m, i));
      g_assert_nonnull (e);
      if (i > 0)
        g_string_append_c (out, ' ');
      g_string_append (out, gbk_entry_display (e));
      if (gbk_entry_is_dir (e))
        g_string_append_c (out, '/');
    }
  return g_string_free (out, false);
}

#define assert_visible(m, expected) \
  do { g_autofree char *got_ = visible_names (m); g_assert_cmpstr (got_, ==, expected); } while (0)

static void
test_order_name (void)
{
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  SNAPSHOT (m, "file10", "File2", "zeta/", "Alpha/", "a.txt", ".hidden", "backup~");
  g_assert_cmpint (gbk_dir_model_state (m), ==, GBK_DIR_READY);
  g_assert_cmpuint (gbk_dir_model_n_entries (m), ==, 7);
  assert_visible (m, "Alpha/ zeta/ a.txt File2 file10");

  gbk_dir_model_set_show_hidden (m, true);
  gbk_dir_model_check_order (m);
  assert_visible (m, "Alpha/ zeta/ .hidden a.txt backup~ File2 file10");
}

static void
test_order_mtime (void)
{
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  /* mtime = index in the list: later names are newer */
  SNAPSHOT (m, "old", "dir1/", "mid", "dir2/", "new");
  gbk_dir_model_set_sort (m, GBK_SORT_MTIME);
  gbk_dir_model_check_order (m);
  assert_visible (m, "dir1/ dir2/ new mid old");   /* dirs first; dir mtimes tie -> name */
}

static GbkEntry *
mk_sized (const char *name, GbkFileKind kind, uint64_t size)
{
  GbkEntryInfo info = { .kind = kind, .size = size, .mode = 0644 };
  GbkEntry *e = gbk_entry_new (name, strlen (name), &info, types);
  g_assert_nonnull (e);
  return e;
}

static void
test_order_size (void)
{
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  GbkScanMsg *s = msg (GBK_SCAN_CHUNK);
  s->first = s->last = s->live = true;
  g_ptr_array_add (s->upserts, mk_sized ("small", GBK_KIND_REGULAR, 10));
  g_ptr_array_add (s->upserts, mk_sized ("zdir", GBK_KIND_DIRECTORY, 9999));
  g_ptr_array_add (s->upserts, mk_sized ("big", GBK_KIND_REGULAR, 5000));
  g_ptr_array_add (s->upserts, mk_sized ("adir", GBK_KIND_DIRECTORY, 1));
  g_ptr_array_add (s->upserts, mk_sized ("same-b", GBK_KIND_REGULAR, 10));
  gbk_dir_model_apply_for_test (m, s);
  gbk_dir_model_set_sort (m, GBK_SORT_SIZE);
  gbk_dir_model_check_order (m);
  /* Folders first by name (their size means nothing); files largest first, ties by name. */
  assert_visible (m, "adir/ zdir/ big same-b small");

  /* A file that grows moves up. */
  GbkScanMsg *d = msg (GBK_SCAN_DELTA);
  g_ptr_array_add (d->upserts, mk_sized ("small", GBK_KIND_REGULAR, 6000));
  gbk_dir_model_apply_for_test (m, d);
  gbk_dir_model_check_order (m);
  assert_visible (m, "adir/ zdir/ small big same-b");
}

static void
test_handles (void)
{
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  SNAPSHOT (m, "a", "b", "c");

  GbkHandle b = gbk_dir_model_find (m, "b");
  g_assert_cmpuint (b, !=, GBK_HANDLE_NONE);
  g_assert_cmpuint (gbk_dir_model_visible_index (m, b), ==, 1);

  GbkScanMsg *d = msg (GBK_SCAN_DELTA);
  g_ptr_array_add (d->removes, g_strdup ("b"));
  gbk_dir_model_apply_for_test (m, d);
  g_assert_null (gbk_dir_model_get (m, b));
  g_assert_cmpuint (gbk_dir_model_visible_index (m, b), ==, SIZE_MAX);

  /* The freed slot is reused; the old handle must still not resolve. */
  d = msg (GBK_SCAN_DELTA);
  g_ptr_array_add (d->upserts, mk ("bb", GBK_KIND_REGULAR, 0));
  gbk_dir_model_apply_for_test (m, d);
  GbkHandle bb = gbk_dir_model_find (m, "bb");
  g_assert_cmpuint (bb & 0xffffffffu, ==, b & 0xffffffffu);   /* same slot... */
  g_assert_cmpuint (bb, !=, b);                               /* ...new generation */
  g_assert_null (gbk_dir_model_get (m, b));
  g_assert_cmpstr (gbk_entry_name (gbk_dir_model_get (m, bb)), ==, "bb");

  g_assert_null (gbk_dir_model_get (m, GBK_HANDLE_NONE));
  g_assert_null (gbk_dir_model_get (m, ((GbkHandle) 1 << 32) | 9999));
  g_assert_cmpuint (gbk_dir_model_find (m, "nope"), ==, GBK_HANDLE_NONE);
}

static void
test_update_keeps_handle (void)
{
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  SNAPSHOT (m, "a", "b");
  GbkHandle a = gbk_dir_model_find (m, "a");

  GbkScanMsg *d = msg (GBK_SCAN_DELTA);
  g_ptr_array_add (d->upserts, mk ("a", GBK_KIND_REGULAR, 999));
  gbk_dir_model_apply_for_test (m, d);
  g_assert_nonnull (gbk_dir_model_get (m, a));
  g_assert_cmpint (gbk_dir_model_get (m, a)->mtime_ns, ==, 999);
  g_assert_cmpuint (gbk_dir_model_n_entries (m), ==, 2);
  gbk_dir_model_check_order (m);
}

static void
test_rescan_sweep (void)
{
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  SNAPSHOT (m, "keep", "drop");
  GbkHandle keep = gbk_dir_model_find (m, "keep");
  GbkHandle drop = gbk_dir_model_find (m, "drop");

  /* A rescan in two chunks: entries stay visible until the last chunk sweeps. */
  GbkScanMsg *c1 = msg (GBK_SCAN_CHUNK);
  c1->first = true;
  g_ptr_array_add (c1->upserts, mk ("keep", GBK_KIND_REGULAR, 5));
  gbk_dir_model_apply_for_test (m, c1);
  g_assert_nonnull (gbk_dir_model_get (m, drop));

  GbkScanMsg *c2 = msg (GBK_SCAN_CHUNK);
  c2->last = true;
  g_ptr_array_add (c2->upserts, mk ("new", GBK_KIND_REGULAR, 6));
  gbk_dir_model_apply_for_test (m, c2);

  g_assert_nonnull (gbk_dir_model_get (m, keep));   /* survived: same handle */
  g_assert_null (gbk_dir_model_get (m, drop));
  assert_visible (m, "keep new");
  gbk_dir_model_check_order (m);
}

static void
test_duplicate_in_message (void)
{
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  GbkScanMsg *s = msg (GBK_SCAN_CHUNK);
  s->first = s->last = true;
  g_ptr_array_add (s->upserts, mk ("x", GBK_KIND_REGULAR, 1));
  g_ptr_array_add (s->upserts, mk ("x", GBK_KIND_REGULAR, 2));
  gbk_dir_model_apply_for_test (m, s);
  g_assert_cmpuint (gbk_dir_model_n_entries (m), ==, 1);
  g_assert_cmpint (gbk_dir_model_get (m, gbk_dir_model_find (m, "x"))->mtime_ns, ==, 2);
  gbk_dir_model_check_order (m);
}

static void
test_error_and_gone (void)
{
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  GbkScanMsg *e = msg (GBK_SCAN_ERROR);
  e->error = EACCES;
  gbk_dir_model_apply_for_test (m, e);
  g_assert_cmpint (gbk_dir_model_state (m), ==, GBK_DIR_ERROR);
  g_assert_cmpint (gbk_dir_model_error (m), ==, EACCES);

  g_autoptr (GbkDirModel) g = gbk_dir_model_new_detached (types);
  SNAPSHOT (g, "a");
  gbk_dir_model_apply_for_test (g, msg (GBK_SCAN_GONE));
  g_assert_cmpint (gbk_dir_model_state (g), ==, GBK_DIR_GONE);
  g_assert_cmpuint (gbk_dir_model_n_visible (g), ==, 1);    /* last known state kept */
}

/* Oracle test: random deltas of random sizes (small ones take the incremental path, large
 * ones the full re-sort). After every batch, check_order compares against a full sort. */
static void
test_random_oracle (void)
{
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  snapshot (m, nullptr, 0);
  GRand *r = g_rand_new_with_seed (12345);
  const char *const stems[] = { "a", "B", "file", "File", "x10", "x9", ".h", "z~", "dir" };

  for (int round = 0; round < 3000; round++)
    {
      GbkScanMsg *d = msg (GBK_SCAN_DELTA);
      int n = g_rand_int_range (r, 0, 100) < 90 ? g_rand_int_range (r, 1, 4)
                                                : g_rand_int_range (r, 50, 300);
      for (int i = 0; i < n; i++)
        {
          g_autofree char *name = g_strdup_printf ("%s%d", stems[g_rand_int_range (r, 0, 9)],
                                                   g_rand_int_range (r, 0, 400));
          if (g_rand_boolean (r))
            g_ptr_array_add (d->removes, g_steal_pointer (&name));
          else
            g_ptr_array_add (d->upserts,
                             mk (name, g_rand_int_range (r, 0, 5) == 0 ? GBK_KIND_DIRECTORY
                                                                       : GBK_KIND_REGULAR,
                                 g_rand_int_range (r, 0, 50)));
        }
      gbk_dir_model_apply_for_test (m, d);
      if (round % 500 == 0)
        gbk_dir_model_set_show_hidden (m, !gbk_dir_model_show_hidden (m));
      if (round % 700 == 0)
        gbk_dir_model_set_sort (m, (GbkSortMode) ((gbk_dir_model_sort (m) + 1) % 3));
      gbk_dir_model_check_order (m);
    }
  g_rand_free (r);
}

static void
on_changed (GbkDirModel *m G_GNUC_UNUSED, int *count)
{
  (*count)++;
}

static void
test_changed_signal (void)
{
  int count = 0;
  g_autoptr (GObject) owner = g_object_new (G_TYPE_OBJECT, nullptr);
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  gbk_connect (owner, m, "changed", G_CALLBACK (on_changed), &count);
  SNAPSHOT (m, "a");
  g_assert_cmpint (count, ==, 1);
  gbk_dir_model_set_show_hidden (m, true);
  g_assert_cmpint (count, ==, 2);
  gbk_dir_model_set_show_hidden (m, true);              /* no-op: no signal */
  g_assert_cmpint (count, ==, 2);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  const char globs[] = "50:text/plain:*.txt\n";
  GbkTypeBuilder *b = gbk_type_builder_new ();
  gbk_type_builder_load_globs2 (b, globs, strlen (globs));
  types = gbk_type_builder_finish (b);

  g_test_add_func ("/dirmodel/order-name", test_order_name);
  g_test_add_func ("/dirmodel/order-mtime", test_order_mtime);
  g_test_add_func ("/dirmodel/order-size", test_order_size);
  g_test_add_func ("/dirmodel/handles", test_handles);
  g_test_add_func ("/dirmodel/update-keeps-handle", test_update_keeps_handle);
  g_test_add_func ("/dirmodel/rescan-sweep", test_rescan_sweep);
  g_test_add_func ("/dirmodel/duplicate-in-message", test_duplicate_in_message);
  g_test_add_func ("/dirmodel/error-and-gone", test_error_and_gone);
  g_test_add_func ("/dirmodel/random-oracle", test_random_oracle);
  g_test_add_func ("/dirmodel/changed-signal", test_changed_signal);
  int status = g_test_run ();
  gbk_type_table_unref (types);
  return status;
}
