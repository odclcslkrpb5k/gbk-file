#include "filetype.h"

static const char fixture[]
    = "# comment\n"
      "50:application/gzip:*.gz\n"
      "50:application/x-compressed-tar:*.tar.gz\n"
      "50:application/zip:*.zip\n"
      "50:text/x-csrc:*.c:cs\n"
      "50:text/x-c++src:*.C:cs\n"
      "50:text/x-chdr:*.h\n"
      "50:video/mp2t:*.ts\n"
      "50:text/vnd.trolltech.linguist:*.ts\n"   /* same weight, later: loses */
      "40:text/plain:*.txt\n"
      "60:text/x-readme:*.txt\n"                /* higher weight: wins */
      "50:text/x-makefile:makefile\n"           /* literal name: ignored */
      "60:application/x-sharedlib:*.so.[0-9]*\n" /* complex glob: ignored */
      "50:application/x-trash:*~\n"             /* not an extension: ignored */
      "50:application/x-shellscript:*.sh\n"
      "50:application/x-appimage:*.AppImage\n"
      "garbage line\n"
      "x:text/plain:*.bad\n"                    /* bad weight */
      "50:notamime:*.bad\n"                     /* bad mime */
      "50:text/plain:*.b/d\n"                   /* bad ext */
      "50:text/plain:*.\n"                      /* empty ext */
      "50:text/plain:*.last";                   /* no trailing newline */

static GbkTypeTable *
fixture_table (void)
{
  GbkTypeBuilder *b = gbk_type_builder_new ();
  g_assert_cmpuint (gbk_type_builder_load_globs2 (b, fixture, strlen (fixture)), ==, 13);
  const char icons[] = "application/zip:package-x-generic\nbad line\n";
  gbk_type_builder_load_generic_icons (b, icons, strlen (icons));
  return gbk_type_builder_finish (b);
}

static const char *
mime_of (const GbkTypeTable *t, const char *name, GbkFileKind kind, bool exec)
{
  return gbk_type_table_mime (t, gbk_type_table_lookup (t, name, strlen (name), kind, exec));
}

#define assert_mime(t, name, expected) \
  g_assert_cmpstr (mime_of (t, name, GBK_KIND_REGULAR, false), ==, expected)

static void
test_basic (void)
{
  g_autoptr (GbkTypeTable) t = fixture_table ();
  assert_mime (t, "a.zip", "application/zip");
  assert_mime (t, "a.gz", "application/gzip");
  assert_mime (t, "a.h", "text/x-chdr");
  assert_mime (t, "noext", "application/octet-stream");
  assert_mime (t, "a.unknownext", "application/octet-stream");
  assert_mime (t, "a.last", "text/plain");
}

static void
test_longest_match (void)
{
  g_autoptr (GbkTypeTable) t = fixture_table ();
  assert_mime (t, "a.tar.gz", "application/x-compressed-tar");
  assert_mime (t, "a.b.tar.gz", "application/x-compressed-tar");
  assert_mime (t, "a.tar.gz.zip", "application/zip");
  assert_mime (t, "tar.gz", "application/gzip");   /* "tar" is the name, not an extension */
}

static void
test_dotfiles_and_edges (void)
{
  g_autoptr (GbkTypeTable) t = fixture_table ();
  assert_mime (t, ".zip", "application/octet-stream");   /* hidden file named "zip" */
  assert_mime (t, ".bashrc", "application/octet-stream");
  assert_mime (t, ".config.zip", "application/zip");
  assert_mime (t, "a.", "application/octet-stream");
  assert_mime (t, "a..zip", "application/zip");
  assert_mime (t, ".", "application/octet-stream");
  assert_mime (t, "makefile", "application/octet-stream");
  assert_mime (t, "x.so.1", "application/octet-stream");
  assert_mime (t, "backup~", "application/octet-stream");
}

static void
test_case (void)
{
  g_autoptr (GbkTypeTable) t = fixture_table ();
  assert_mime (t, "A.ZIP", "application/zip");
  assert_mime (t, "a.Tar.Gz", "application/x-compressed-tar");
  assert_mime (t, "a.H", "text/x-chdr");
  assert_mime (t, "a.c", "text/x-csrc");             /* cs: exact */
  assert_mime (t, "a.C", "text/x-c++src");           /* cs: exact */
  assert_mime (t, "x.appimage", "application/x-appimage");
}

static void
test_weights (void)
{
  g_autoptr (GbkTypeTable) t = fixture_table ();
  assert_mime (t, "a.ts", "video/mp2t");
  assert_mime (t, "a.txt", "text/x-readme");
}

static void
test_kinds_and_exec (void)
{
  g_autoptr (GbkTypeTable) t = fixture_table ();
  g_assert_cmpstr (mime_of (t, "dir.zip", GBK_KIND_DIRECTORY, false), ==, "inode/directory");
  g_assert_cmpstr (mime_of (t, "x", GBK_KIND_FIFO, false), ==, "inode/fifo");
  g_assert_cmpstr (mime_of (t, "x", GBK_KIND_BROKEN_LINK, false), ==, "inode/symlink");
  g_assert_cmpstr (mime_of (t, "prog", GBK_KIND_REGULAR, true), ==, "application/x-executable");
  g_assert_cmpstr (mime_of (t, "run.bin", GBK_KIND_REGULAR, true), ==, "application/x-executable");
  /* a known extension wins over the exec bit: scripts stay scripts */
  g_assert_cmpstr (mime_of (t, "go.sh", GBK_KIND_REGULAR, true), ==, "application/x-shellscript");
}

static void
test_non_utf8_names (void)
{
  g_autoptr (GbkTypeTable) t = fixture_table ();
  const char name[] = "\xff\xfe\x80.zip";
  g_assert_cmpstr (gbk_type_table_mime (t, gbk_type_table_lookup (t, name, sizeof name - 1,
                                                                  GBK_KIND_REGULAR, false)),
                   ==, "application/zip");
  const char bad_ext[] = "a.\xff\xfe";
  g_assert_cmpstr (gbk_type_table_mime (t, gbk_type_table_lookup (t, bad_ext, sizeof bad_ext - 1,
                                                                  GBK_KIND_REGULAR, false)),
                   ==, "application/octet-stream");
}

static void
test_overrides (void)
{
  GbkTypeBuilder *b = gbk_type_builder_new ();
  gbk_type_builder_load_globs2 (b, fixture, strlen (fixture));
  g_assert_true (gbk_type_builder_add (b, "zip", "application/x-mine", 1000, false));
  g_assert_true (gbk_type_builder_add (b, "weird", "application/octet-stream", 1000, false));
  g_assert_false (gbk_type_builder_add (b, "", "text/plain", 50, false));
  g_assert_false (gbk_type_builder_add (b, "a/b", "text/plain", 50, false));
  g_assert_false (gbk_type_builder_add (b, "ok", "no slash", 50, false));
  g_assert_false (gbk_type_builder_add (b, "ok", "text/pl ain", 50, false));
  g_autofree char *long_ext = g_strnfill (GBK_TYPE_MAX_EXT + 1, 'x');
  g_assert_false (gbk_type_builder_add (b, long_ext, "text/plain", 50, false));
  g_autoptr (GbkTypeTable) t = gbk_type_builder_finish (b);

  assert_mime (t, "a.zip", "application/x-mine");
  /* explicit mapping to unknown must not fall through to the shorter extension */
  assert_mime (t, "a.gz.weird", "application/octet-stream");
}

static void
test_icons (void)
{
  g_autoptr (GbkTypeTable) t = fixture_table ();
  GbkTypeId zip = gbk_type_table_lookup (t, "a.zip", 5, GBK_KIND_REGULAR, false);
  g_assert_cmpstr (gbk_type_table_icon (t, zip), ==, "application-zip");
  g_assert_cmpstr (gbk_type_table_generic_icon (t, zip), ==, "package-x-generic");
  GbkTypeId ts = gbk_type_table_lookup (t, "a.ts", 4, GBK_KIND_REGULAR, false);
  g_assert_cmpstr (gbk_type_table_generic_icon (t, ts), ==, "video-x-generic");
  GbkTypeId h = gbk_type_table_lookup (t, "a.h", 3, GBK_KIND_REGULAR, false);
  g_assert_cmpstr (gbk_type_table_generic_icon (t, h), ==, "text-x-generic");
  g_assert_cmpstr (gbk_type_table_generic_icon (t, GBK_TYPE_DIRECTORY), ==, "folder");
  GbkTypeId sh = gbk_type_table_lookup (t, "a.sh", 4, GBK_KIND_REGULAR, false);
  g_assert_cmpstr (gbk_type_table_generic_icon (t, sh), ==, "application-x-generic");
}

static void *
hammer (void *data)
{
  GbkTypeTable *t = data;
  for (int i = 0; i < 20000; i++)
    {
      GbkTypeId id = gbk_type_table_lookup (t, "x.tar.gz", 8, GBK_KIND_REGULAR, false);
      g_assert_cmpstr (gbk_type_table_mime (t, id), ==, "application/x-compressed-tar");
    }
  gbk_type_table_unref (t);
  return nullptr;
}

static void
test_threads (void)
{
  GbkTypeTable *t = fixture_table ();
  GThread *threads[4];
  for (size_t i = 0; i < G_N_ELEMENTS (threads); i++)
    threads[i] = g_thread_new ("lookup", hammer, gbk_type_table_ref (t));
  gbk_type_table_unref (t);           /* threads hold the last refs */
  for (size_t i = 0; i < G_N_ELEMENTS (threads); i++)
    g_thread_join (threads[i]);
}

static void
test_system (void)
{
  g_autoptr (GbkTypeTable) t = gbk_type_table_new_system ();
  if (gbk_type_table_size (t) == GBK_TYPE_N_BUILTIN)
    {
      g_test_skip ("no shared-mime-info database");
      return;
    }
  assert_mime (t, "a.tar.gz", "application/x-compressed-tar");
  assert_mime (t, "a.c", "text/x-csrc");
  assert_mime (t, "a.C", "text/x-c++src");
  assert_mime (t, "A.PNG", "image/png");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  g_test_add_func ("/filetype/basic", test_basic);
  g_test_add_func ("/filetype/longest-match", test_longest_match);
  g_test_add_func ("/filetype/dotfiles-and-edges", test_dotfiles_and_edges);
  g_test_add_func ("/filetype/case", test_case);
  g_test_add_func ("/filetype/weights", test_weights);
  g_test_add_func ("/filetype/kinds-and-exec", test_kinds_and_exec);
  g_test_add_func ("/filetype/non-utf8", test_non_utf8_names);
  g_test_add_func ("/filetype/overrides", test_overrides);
  g_test_add_func ("/filetype/icons", test_icons);
  g_test_add_func ("/filetype/threads", test_threads);
  g_test_add_func ("/filetype/system", test_system);
  return g_test_run ();
}
