/* Context menu contents (pure). */
#include "menu.h"

static GbkConfig *config;

static GbkItem *
item (const char *path, const char *mime, bool dir, bool exec, bool script)
{
  GbkItem *it = g_new0 (GbkItem, 1);
  it->path = g_strdup (path);
  it->display = g_strdup (path);
  it->mime = g_strdup (mime);
  it->is_dir = dir;
  it->executable = exec;
  it->script = script;
  return it;
}

static char *
describe (GPtrArray *items, bool background)
{
  static const char *const names[] = {
    [GBK_MENU_SEPARATOR] = "|", [GBK_MENU_OPEN] = "open", [GBK_MENU_OPEN_WITH] = "with",
    [GBK_MENU_EXECUTE] = "exec", [GBK_MENU_EXECUTE_TERMINAL] = "exec-term",
    [GBK_MENU_TERMINAL_HERE] = "term-here", [GBK_MENU_ACTION] = "action",
    [GBK_MENU_SORT_NAME] = "sort-name", [GBK_MENU_SORT_MTIME] = "sort-mtime",
    [GBK_MENU_SHOW_HIDDEN] = "hidden",
  };
  g_autoptr (GArray) entries = gbk_menu_entries (config, items, background);
  GString *s = g_string_new ("");
  for (guint i = 0; i < entries->len; i++)
    {
      const GbkMenuEntry *e = &g_array_index (entries, GbkMenuEntry, i);
      if (i > 0)
        g_string_append_c (s, ' ');
      if (e->kind == GBK_MENU_ACTION)
        g_string_append_printf (s, "action:%s", e->action->id);
      else
        g_string_append (s, names[e->kind]);
    }
  return g_string_free (s, false);
}

static GPtrArray *
items_of (GbkItem *first, ...)
{
  GPtrArray *a = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_item_free);
  va_list ap;
  va_start (ap, first);
  for (GbkItem *it = first; it != nullptr; it = va_arg (ap, GbkItem *))
    g_ptr_array_add (a, it);
  va_end (ap);
  return a;
}

#define assert_menu(items, bg, expected) \
  do { g_autoptr (GPtrArray) i_ = items; g_autofree char *d_ = describe (i_, bg); \
       g_assert_cmpstr (d_, ==, expected); } while (0)

static void
test_menus (void)
{
  assert_menu (items_of (item ("/d/a.zip", "application/zip", false, false, false), (GbkItem *) nullptr), false,
               "open with | action:extract action:compress");
  assert_menu (items_of (item ("/d/go.sh", "application/x-shellscript", false, true, true), (GbkItem *) nullptr),
               false, "open with exec exec-term | action:compress");
  assert_menu (items_of (item ("/d/go.sh", "application/x-shellscript", false, false, true), (GbkItem *) nullptr),
               false, "open with exec exec-term | action:compress");
  assert_menu (items_of (item ("/d/prog", "application/x-executable", false, true, false), (GbkItem *) nullptr),
               false, "open with exec exec-term | action:compress");
  assert_menu (items_of (item ("/d/sub", "inode/directory", true, false, false), (GbkItem *) nullptr), false,
               "open term-here | action:compress");
  /* Two zips: same type, extract applies to both; no Execute for multiple. */
  assert_menu (items_of (item ("/d/a.zip", "application/zip", false, false, false),
                         item ("/d/b.zip", "application/zip", false, false, false), (GbkItem *) nullptr),
               false, "open with | action:extract action:compress");
  /* Mixed: no Open With, and extract doesn't apply to the text file. */
  assert_menu (items_of (item ("/d/a.zip", "application/zip", false, false, false),
                         item ("/d/t.txt", "text/plain", false, false, false), (GbkItem *) nullptr),
               false, "open | action:compress");
  /* Background: the current directory. */
  assert_menu (items_of (item ("/d", "inode/directory", true, false, false), (GbkItem *) nullptr), true,
               "term-here | action:compress | sort-name sort-mtime | hidden");
}

static void
test_no_actions_no_dangling_separator (void)
{
  gbk_config_free (config);
  config = gbk_config_new_default ();
  assert_menu (items_of (item ("/d/t.txt", "text/plain", false, false, false), (GbkItem *) nullptr), false,
               "open with");
  assert_menu (items_of (item ("/d", "inode/directory", true, false, false), (GbkItem *) nullptr), true,
               "term-here | sort-name sort-mtime | hidden");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  const char conf[] =
    "[Action extract]\nName=Extract\nTypes=application/zip;\nCommand=x %F\n"
    "[Action compress]\nName=Compress\nTypes=*\nDirectories=true\nCommand=c %F\n";
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func (g_free);
  config = gbk_config_load_data (conf, strlen (conf), errors);
  g_assert_cmpuint (errors->len, ==, 0);
  g_test_add_func ("/menu/contents", test_menus);
  g_test_add_func ("/menu/no-actions", test_no_actions_no_dangling_separator);
  int status = g_test_run ();
  gbk_config_free (config);
  return status;
}
