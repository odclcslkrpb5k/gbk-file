/* File operations on real temp directories, with scripted answers to every prompt. */
#include "fileops.h"

static char *root;

typedef struct
{
  GbkChoice answer;       /* for conflicts */
  bool apply_all;
  int conflicts;
  int errors;
  GbkChoice error_answer;
  bool delete_untrashable;
  int cancel_after_progress;   /* >0: report cancelled after this many progress calls */
  int progress_calls;
} Script;

static GbkChoice
on_conflict (const char *src G_GNUC_UNUSED, const char *dst G_GNUC_UNUSED,
             bool dst_is_dir G_GNUC_UNUSED, bool *apply_all, void *data)
{
  Script *s = data;
  s->conflicts++;
  *apply_all = s->apply_all;
  return s->answer;
}

static GbkChoice
on_error (const char *path G_GNUC_UNUSED, const GError *error, void *data)
{
  Script *s = data;
  s->errors++;
  g_test_message ("op error: %s", error->message);
  return s->error_answer;
}

static bool
on_untrashable (const char *path G_GNUC_UNUSED, void *data)
{
  return ((Script *) data)->delete_untrashable;
}

static void
on_progress (uint64_t done G_GNUC_UNUSED, uint64_t total G_GNUC_UNUSED,
             const char *current G_GNUC_UNUSED, void *data)
{
  ((Script *) data)->progress_calls++;
}

static bool
is_cancelled (void *data)
{
  Script *s = data;
  return s->cancel_after_progress > 0 && s->progress_calls >= s->cancel_after_progress;
}

static GbkOpsHooks
hooks (Script *s)
{
  return (GbkOpsHooks) { on_conflict, on_error, on_untrashable, on_progress, is_cancelled, s };
}

/* ---- fixture helpers ---- */

static char *
P (const char *rel)
{
  return g_build_filename (root, rel, nullptr);
}

static void
mk (const char *rel, const char *content)
{
  g_autofree char *p = P (rel);
  g_autofree char *dir = g_path_get_dirname (p);
  g_mkdir_with_parents (dir, 0755);
  if (g_str_has_suffix (rel, "/"))
    g_mkdir_with_parents (p, 0755);
  else
    g_assert_true (g_file_set_contents (p, content, -1, nullptr));
}

static char *
slurp (const char *rel)
{
  g_autofree char *p = P (rel);
  char *data = nullptr;
  if (!g_file_get_contents (p, &data, nullptr, nullptr))
    return g_strdup ("(missing)");
  return data;
}

static bool
exists (const char *rel)
{
  g_autofree char *p = P (rel);
  struct stat st;
  return lstat (p, &st) == 0;
}

static GPtrArray *
paths (const char *first, ...)
{
  GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
  va_list ap;
  va_start (ap, first);
  for (const char *r = first; r != nullptr; r = va_arg (ap, const char *))
    g_ptr_array_add (a, P (r));
  va_end (ap);
  return a;
}

static bool
run (GbkOpKind kind, GPtrArray *src, const char *dest_rel, Script *s, GPtrArray *created)
{
  g_autofree char *dest = dest_rel != nullptr ? P (dest_rel) : nullptr;
  GbkOpsHooks h = hooks (s);
  g_autoptr (GError) error = nullptr;
  bool ok = gbk_ops_run (kind, src, dest, &h, created, &error);
  if (!ok)
    g_test_message ("run failed: %s", error->message);
  return ok;
}

#define assert_content(rel, expected) \
  do { g_autofree char *c_ = slurp (rel); g_assert_cmpstr (c_, ==, expected); } while (0)

/* ---- tests ---- */

static void
test_copy_tree (void)
{
  mk ("src/a.txt", "A");
  mk ("src/dir/b.txt", "B");
  mk ("src/dir/deeper/c.txt", "C");
  mk ("dst/", "");
  g_autofree char *link = P ("src/link");
  g_assert_cmpint (symlink ("a.txt", link), ==, 0);
  g_autofree char *a = P ("src/a.txt");
  chmod (a, 0751);

  Script s = { .answer = GBK_CHOICE_CANCEL };
  g_autoptr (GPtrArray) created = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) src = paths ("src/a.txt", "src/dir", "src/link", (char *) nullptr);
  g_assert_true (run (GBK_OP_COPY, src, "dst", &s, created));
  g_assert_cmpint (s.conflicts, ==, 0);
  g_assert_cmpint (s.errors, ==, 0);
  assert_content ("dst/a.txt", "A");
  assert_content ("dst/dir/deeper/c.txt", "C");
  g_assert_cmpuint (created->len, ==, 3);

  struct stat st;
  g_autofree char *da = P ("dst/a.txt");
  g_assert_cmpint (stat (da, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 07777, ==, 0751);          /* mode preserved */
  g_autofree char *dl = P ("dst/link");
  g_autofree char *target = g_file_read_link (dl, nullptr);
  g_assert_cmpstr (target, ==, "a.txt");                   /* symlink copied as a link */
  g_assert_true (s.progress_calls > 0);
}

static void
test_copy_conflicts (void)
{
  mk ("src/f.txt", "new");
  mk ("dst/f.txt", "old");
  g_autoptr (GPtrArray) src = paths ("src/f.txt", (char *) nullptr);

  Script skip = { .answer = GBK_CHOICE_SKIP };
  g_assert_true (run (GBK_OP_COPY, src, "dst", &skip, nullptr));
  g_assert_cmpint (skip.conflicts, ==, 1);
  assert_content ("dst/f.txt", "old");

  Script both = { .answer = GBK_CHOICE_KEEP_BOTH };
  g_assert_true (run (GBK_OP_COPY, src, "dst", &both, nullptr));
  assert_content ("dst/f.txt", "old");
  assert_content ("dst/f (1).txt", "new");

  Script replace = { .answer = GBK_CHOICE_REPLACE };
  g_assert_true (run (GBK_OP_COPY, src, "dst", &replace, nullptr));
  assert_content ("dst/f.txt", "new");

  Script cancel = { .answer = GBK_CHOICE_CANCEL };
  mk ("src/f.txt", "newer");
  g_assert_false (run (GBK_OP_COPY, src, "dst", &cancel, nullptr));
  assert_content ("dst/f.txt", "new");

  /* No temp files left behind by any of it. */
  g_autofree char *dst = P ("dst");
  g_autoptr (GDir) d = g_dir_open (dst, 0, nullptr);
  const char *name;
  while ((name = g_dir_read_name (d)) != nullptr)
    g_assert_false (g_str_has_prefix (name, ".gbk-tmp-"));
}

static void
test_copy_merge_and_apply_all (void)
{
  mk ("src/d/one", "1");
  mk ("src/d/two", "2");
  mk ("dst/d/one", "old1");
  mk ("dst/d/two", "old2");
  mk ("dst/d/keep", "k");
  g_autoptr (GPtrArray) src = paths ("src/d", (char *) nullptr);
  Script s = { .answer = GBK_CHOICE_REPLACE, .apply_all = true };
  g_assert_true (run (GBK_OP_COPY, src, "dst", &s, nullptr));
  g_assert_cmpint (s.conflicts, ==, 1);    /* asked once (the folder), applied to all */
  assert_content ("dst/d/one", "1");
  assert_content ("dst/d/two", "2");
  assert_content ("dst/d/keep", "k");      /* merge, not wipe */
}

static void
test_copy_onto_itself_and_into_itself (void)
{
  mk ("here/x.txt", "X");
  mk ("here/sub/", "");
  g_autoptr (GPtrArray) file = paths ("here/x.txt", (char *) nullptr);
  Script s = { .answer = GBK_CHOICE_CANCEL };
  g_assert_true (run (GBK_OP_COPY, file, "here", &s, nullptr));   /* paste in same folder */
  g_assert_cmpint (s.conflicts, ==, 0);
  assert_content ("here/x (1).txt", "X");

  g_autoptr (GPtrArray) dir = paths ("here", (char *) nullptr);
  g_assert_false (run (GBK_OP_COPY, dir, "here/sub", &s, nullptr));
  g_assert_false (run (GBK_OP_MOVE, dir, "here/sub", &s, nullptr));
  g_assert_false (run (GBK_OP_COPY, dir, "here", &s, nullptr));
  g_assert_true (exists ("here/x.txt"));
}

static void
test_move (void)
{
  mk ("src/m.txt", "M");
  mk ("src/md/inner", "I");
  mk ("dst/", "");
  g_autoptr (GPtrArray) src = paths ("src/m.txt", "src/md", (char *) nullptr);
  Script s = { .answer = GBK_CHOICE_CANCEL };
  g_autoptr (GPtrArray) created = g_ptr_array_new_with_free_func (g_free);
  g_assert_true (run (GBK_OP_MOVE, src, "dst", &s, created));
  g_assert_false (exists ("src/m.txt"));
  g_assert_false (exists ("src/md"));
  assert_content ("dst/m.txt", "M");
  assert_content ("dst/md/inner", "I");
  g_assert_cmpuint (created->len, ==, 2);

  /* Conflict: keep both. */
  mk ("src/m.txt", "M2");
  Script both = { .answer = GBK_CHOICE_KEEP_BOTH };
  g_autoptr (GPtrArray) again = paths ("src/m.txt", (char *) nullptr);
  g_assert_true (run (GBK_OP_MOVE, again, "dst", &both, nullptr));
  assert_content ("dst/m.txt", "M");
  assert_content ("dst/m (1).txt", "M2");
}

static void
test_delete_does_not_follow_symlinks (void)
{
  mk ("victim/precious.txt", "keep me");
  mk ("trash-me/a/b", "x");
  g_autofree char *link = P ("trash-me/a/link-to-victim");
  g_autofree char *victim = P ("victim");
  g_assert_cmpint (symlink (victim, link), ==, 0);

  g_autoptr (GPtrArray) src = paths ("trash-me", (char *) nullptr);
  Script s = { 0 };
  g_assert_true (run (GBK_OP_DELETE, src, nullptr, &s, nullptr));
  g_assert_false (exists ("trash-me"));
  assert_content ("victim/precious.txt", "keep me");
}

static void
test_trash (void)
{
  mk ("t/gone.txt", "bye");
  g_autoptr (GPtrArray) src = paths ("t/gone.txt", (char *) nullptr);
  Script s = { .error_answer = GBK_CHOICE_SKIP };
  g_assert_true (run (GBK_OP_TRASH, src, nullptr, &s, nullptr));
  g_assert_false (exists ("t/gone.txt"));
  g_assert_cmpint (s.errors, ==, 0);
  g_autofree char *trashed = g_build_filename (g_get_user_data_dir (), "Trash", "files",
                                               "gone.txt", nullptr);
  g_assert_true (g_file_test (trashed, G_FILE_TEST_EXISTS));
}

static void
test_errors_and_cancel (void)
{
  mk ("src/ok1", "1");
  mk ("src/ok2", "2");
  mk ("dst/", "");
  g_autoptr (GPtrArray) src = paths ("src/missing", "src/ok1", "src/ok2", (char *) nullptr);

  Script skip = { .error_answer = GBK_CHOICE_SKIP };
  g_assert_true (run (GBK_OP_COPY, src, "dst", &skip, nullptr));
  g_assert_cmpint (skip.errors, ==, 1);
  g_assert_true (exists ("dst/ok1"));

  Script stop = { .error_answer = GBK_CHOICE_CANCEL };
  g_autoptr (GPtrArray) src2 = paths ("src/missing", "src/ok1", (char *) nullptr);
  mk ("dst2/", "");
  g_assert_false (run (GBK_OP_COPY, src2, "dst2", &stop, nullptr));
  g_assert_false (exists ("dst2/ok1"));    /* stopped before it */

  /* Cancel mid-copy of a big file: no partial file at the destination. */
  g_autofree char *big = P ("src/big");
  {
    g_autofree char *chunk = g_malloc0 (1 << 20);
    FILE *f = fopen (big, "wb");
    g_assert_nonnull (f);
    for (int i = 0; i < 64; i++)
      fwrite (chunk, 1, 1 << 20, f);
    fclose (f);
  }
  mk ("dst3/", "");
  g_autoptr (GPtrArray) src3 = paths ("src/big", (char *) nullptr);
  Script cancel = { .cancel_after_progress = 3 };
  g_assert_false (run (GBK_OP_COPY, src3, "dst3", &cancel, nullptr));
  g_assert_false (exists ("dst3/big"));
  g_autofree char *dst3 = P ("dst3");
  g_autoptr (GDir) d = g_dir_open (dst3, 0, nullptr);
  g_assert_null (g_dir_read_name (d));      /* temp file cleaned up too */
}

static void
test_rename (void)
{
  mk ("r/a.txt", "A");
  mk ("r/b.txt", "B");
  g_autofree char *a = P ("r/a.txt");
  g_autoptr (GError) e1 = nullptr;
  g_assert_false (gbk_ops_rename (a, "b.txt", &e1));       /* never replaces */
  g_assert_error (e1, G_IO_ERROR, G_IO_ERROR_EXISTS);
  assert_content ("r/b.txt", "B");

  g_autoptr (GError) e2 = nullptr;
  g_assert_false (gbk_ops_rename (a, "x/y", &e2));
  g_assert_false (gbk_ops_rename (a, "..", nullptr));
  g_assert_false (gbk_ops_rename (a, "", nullptr));
  g_assert_true (gbk_ops_rename (a, "c.txt", nullptr));
  assert_content ("r/c.txt", "A");
  g_assert_false (exists ("r/a.txt"));
}

static void
test_free_name (void)
{
  mk ("n/x.tar.gz", "");
  mk ("n/x (1).tar.gz", "");
  mk ("n/.bashrc", "");
  g_autofree char *dir = P ("n");
  g_autofree char *n1 = gbk_ops_free_name (dir, "x.tar.gz");
  g_assert_cmpstr (n1, ==, "x (2).tar.gz");
  g_autofree char *n2 = gbk_ops_free_name (dir, ".bashrc");
  g_assert_cmpstr (n2, ==, ".bashrc (1)");
  g_autofree char *n3 = gbk_ops_free_name (dir, "plain");
  g_assert_cmpstr (n3, ==, "plain (1)");
}

static void
fresh_root (void)
{
  /* Each test gets its own directory under the session root. */
  static int n;
  g_autofree char *base = g_strdup_printf ("t%d", n++);
  g_autofree char *dir = g_build_filename (g_getenv ("GBK_TEST_ROOT"), base, nullptr);
  g_mkdir_with_parents (dir, 0755);
  g_free (root);
  root = g_steal_pointer (&dir);
}

#define ADD(path, fn) \
  g_test_add_func (path, fn##_wrapped)
#define WRAP(fn) static void fn##_wrapped (void) { fresh_root (); fn (); }
WRAP (test_copy_tree)
WRAP (test_copy_conflicts)
WRAP (test_copy_merge_and_apply_all)
WRAP (test_copy_onto_itself_and_into_itself)
WRAP (test_move)
WRAP (test_delete_does_not_follow_symlinks)
WRAP (test_trash)
WRAP (test_errors_and_cancel)
WRAP (test_rename)
WRAP (test_free_name)

int
main (int argc, char **argv)
{
  /* Private data dir so trash lands somewhere we own (and on the same filesystem). */
  g_autofree char *session = g_dir_make_tmp ("gbk-ops-XXXXXX", nullptr);
  g_autofree char *data = g_build_filename (session, "data", nullptr);
  g_mkdir_with_parents (data, 0700);
  g_setenv ("XDG_DATA_HOME", data, true);
  g_setenv ("GBK_TEST_ROOT", session, true);

  g_test_init (&argc, &argv, nullptr);
  ADD ("/fileops/copy-tree", test_copy_tree);
  ADD ("/fileops/copy-conflicts", test_copy_conflicts);
  ADD ("/fileops/copy-merge-apply-all", test_copy_merge_and_apply_all);
  ADD ("/fileops/onto-and-into-itself", test_copy_onto_itself_and_into_itself);
  ADD ("/fileops/move", test_move);
  ADD ("/fileops/delete-no-follow", test_delete_does_not_follow_symlinks);
  ADD ("/fileops/trash", test_trash);
  ADD ("/fileops/errors-and-cancel", test_errors_and_cancel);
  ADD ("/fileops/rename", test_rename);
  ADD ("/fileops/free-name", test_free_name);
  int status = g_test_run ();
  g_free (root);
  return status;
}
