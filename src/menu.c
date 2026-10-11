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

GArray *
gbk_menu_entries (const GbkConfig *config, GPtrArray *items, bool background)
{
  GArray *out = g_array_new (false, false, sizeof (GbkMenuEntry));
  const GbkItem *only = items->len == 1 ? g_ptr_array_index (items, 0) : nullptr;

  if (background)
    {
      add (out, GBK_MENU_PASTE, nullptr);
      add (out, GBK_MENU_SEPARATOR, nullptr);
      add (out, GBK_MENU_TERMINAL_HERE, nullptr);
    }
  else
    {
      add (out, GBK_MENU_OPEN, nullptr);
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
      add (out, GBK_MENU_SEPARATOR, nullptr);
      add (out, GBK_MENU_TRASH, nullptr);
      add (out, GBK_MENU_DELETE, nullptr);
      if (only != nullptr)
        add (out, GBK_MENU_RENAME, nullptr);
    }

  add (out, GBK_MENU_SEPARATOR, nullptr);
  for (guint i = 0; i < config->actions->len; i++)
    {
      GbkAction *a = g_ptr_array_index (config->actions, i);
      if (action_matches_all (a, items))
        add (out, GBK_MENU_ACTION, a);
    }

  if (background)
    {
      add (out, GBK_MENU_SEPARATOR, nullptr);
      add (out, GBK_MENU_SORT_NAME, nullptr);
      add (out, GBK_MENU_SORT_MTIME, nullptr);
      add (out, GBK_MENU_SEPARATOR, nullptr);
      add (out, GBK_MENU_SHOW_HIDDEN, nullptr);
    }

  if (out->len > 0 && g_array_index (out, GbkMenuEntry, out->len - 1).kind == GBK_MENU_SEPARATOR)
    g_array_set_size (out, out->len - 1);
  return out;
}
