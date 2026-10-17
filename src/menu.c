#include "menu.h"

static void
add (GArray *out, GbkMenuKind kind, GbkAction *action)
{
  if (kind == GBK_MENU_SEPARATOR
      && (out->len == 0 || g_array_index (out, GbkMenuEntry, out->len - 1).kind == GBK_MENU_SEPARATOR))
    return;
  GbkMenuEntry e = { kind, action };
  g_array_append_val (out, e);
}

const char *
gbk_menu_common_mime (GPtrArray *items)
{
  if (items->len == 0)
    return nullptr;
  const GbkItem *first = g_ptr_array_index (items, 0);
  for (guint i = 0; i < items->len; i++)
    {
      const GbkItem *it = g_ptr_array_index (items, i);
      if (it->is_dir || !g_str_equal (it->mime, first->mime))
        return nullptr;
    }
  return first->mime;
}

static bool
action_matches_all (const GbkAction *a, GPtrArray *items)
{
  for (guint i = 0; i < items->len; i++)
    {
      const GbkItem *it = g_ptr_array_index (items, i);
      if (!gbk_action_matches (a, it->mime, it->is_dir))
        return false;
    }
  return items->len > 0;
}

static bool
all_regular (const GPtrArray *items)
{
  for (guint i = 0; i < items->len; i++)
    if (!((const GbkItem *) g_ptr_array_index (items, i))->regular)
      return false;
  return items->len > 0;
}

static bool
all_dirs (const GPtrArray *items)
{
  for (guint i = 0; i < items->len; i++)
    if (!((const GbkItem *) g_ptr_array_index (items, i))->is_dir)
      return false;
  return items->len > 0;
}

GArray *
gbk_menu_entries (const GbkConfig *config, GPtrArray *items, GbkMenuTarget target)
{
  GArray *out = g_array_new (false, false, sizeof (GbkMenuEntry));
  const GbkItem *only = items->len == 1 ? g_ptr_array_index (items, 0) : nullptr;

  switch (target)
    {
    case GBK_MENU_ON_BACKGROUND:
      add (out, GBK_MENU_CREATE_FOLDER, nullptr);
      add (out, GBK_MENU_CREATE_FILE, nullptr);
      add (out, GBK_MENU_SEPARATOR, nullptr);
      add (out, GBK_MENU_PASTE, nullptr);
      add (out, GBK_MENU_COPY_PATH, nullptr);
      add (out, GBK_MENU_SEPARATOR, nullptr);
      add (out, GBK_MENU_TERMINAL_HERE, nullptr);
      break;
    case GBK_MENU_ON_PATHBAR:
      if (only == nullptr || !only->is_dir)
        gbk_fatal ("path bar menu needs exactly one folder");
      add (out, GBK_MENU_OPEN, nullptr);
      add (out, GBK_MENU_OPEN_TAB, nullptr);
      add (out, GBK_MENU_OPEN_WINDOW, nullptr);
      add (out, GBK_MENU_TERMINAL_HERE, nullptr);
      add (out, GBK_MENU_SEPARATOR, nullptr);
      add (out, GBK_MENU_COPY, nullptr);
      add (out, GBK_MENU_PASTE_INTO, nullptr);
      add (out, GBK_MENU_COPY_PATH, nullptr);
      break;
    case GBK_MENU_ON_ITEMS:
      add (out, GBK_MENU_OPEN, nullptr);
      if (all_dirs (items))
        {
          add (out, GBK_MENU_OPEN_TAB, nullptr);
          add (out, GBK_MENU_OPEN_WINDOW, nullptr);
        }
      if (gbk_menu_common_mime (items) != nullptr)
        add (out, GBK_MENU_OPEN_WITH, nullptr);
      if (only != nullptr && gbk_open_can_execute (only))
        {
          add (out, GBK_MENU_EXECUTE, nullptr);
          add (out, GBK_MENU_EXECUTE_TERMINAL, nullptr);
        }
      if (only != nullptr && only->is_dir)
        add (out, GBK_MENU_TERMINAL_HERE, nullptr);
      add (out, GBK_MENU_SEPARATOR, nullptr);
      add (out, GBK_MENU_CUT, nullptr);
      add (out, GBK_MENU_COPY, nullptr);
      if (only != nullptr && only->is_dir)
        add (out, GBK_MENU_PASTE_INTO, nullptr);
      add (out, GBK_MENU_COPY_PATH, nullptr);
      add (out, GBK_MENU_SEPARATOR, nullptr);
      add (out, GBK_MENU_TRASH, nullptr);
      add (out, GBK_MENU_DELETE, nullptr);
      if (only != nullptr)
        add (out, GBK_MENU_RENAME, nullptr);
      if (all_regular (items))
        {
          add (out, GBK_MENU_SEPARATOR, nullptr);
          add (out, GBK_MENU_EXECUTABLE, nullptr);
        }
      break;
    default:
      gbk_fatal ("bad menu target %d", (int) target);
    }

  add (out, GBK_MENU_SEPARATOR, nullptr);
  for (guint i = 0; i < config->actions->len; i++)
    {
      GbkAction *a = g_ptr_array_index (config->actions, i);
      if (action_matches_all (a, items))
        add (out, GBK_MENU_ACTION, a);
    }

  if (target == GBK_MENU_ON_BACKGROUND)
    {
      add (out, GBK_MENU_SEPARATOR, nullptr);
      add (out, GBK_MENU_SORT_NAME, nullptr);
      add (out, GBK_MENU_SORT_MTIME, nullptr);
      add (out, GBK_MENU_SORT_SIZE, nullptr);
      add (out, GBK_MENU_SEPARATOR, nullptr);
      add (out, GBK_MENU_SHOW_HIDDEN, nullptr);
    }

  if (only != nullptr)
    {
      add (out, GBK_MENU_SEPARATOR, nullptr);
      add (out, GBK_MENU_PROPERTIES, nullptr);
    }

  if (out->len > 0 && g_array_index (out, GbkMenuEntry, out->len - 1).kind == GBK_MENU_SEPARATOR)
    g_array_set_size (out, out->len - 1);
  return out;
}
