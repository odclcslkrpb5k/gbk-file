#include "window.h"
#include "lifetime.h"
#include "menu.h"
#include "open.h"

#define HISTORY_MAX 100

struct _GbkWindow
{
  GtkApplicationWindow parent_instance;

  GbkContext *ctx;
  GbkView *view;
  GtkLabel *status;

  char *path;
  GbkDirModel *model;
  gulong model_state_id;
  gulong model_changed_id;

  GPtrArray *back;        /* char* paths, most recent last */
  GPtrArray *forward;

  bool show_hidden;
  GbkSortMode sort;
  char *focus_on_load;    /* raw name to select once it appears (after going up) */
  char *message;          /* transient error shown in the status bar */
};

G_DEFINE_FINAL_TYPE (GbkWindow, gbk_window, GTK_TYPE_APPLICATION_WINDOW)

/* ---- status bar ---- */

static char *
format_size (uint64_t bytes)
{
  return g_format_size_full (bytes, G_FORMAT_SIZE_IEC_UNITS);
}

static void
update_status (GbkWindow *w)
{
  g_autoptr (GString) s = g_string_new ("");
  GbkDirModel *m = w->model;
  const char *search = gbk_view_type_ahead_text (w->view);

  if (w->message != nullptr)
    g_string_append (s, w->message);
  else if (search[0] != '\0')
    g_string_append_printf (s, "Find: %s", search);
  else if (m == nullptr)
    ;
  else if (gbk_dir_model_state (m) == GBK_DIR_ERROR)
    g_string_append_printf (s, "Can't open: %s", g_strerror (gbk_dir_model_error (m)));
  else
    {
      size_t n_sel;
      g_autofree GbkHandle *sel = gbk_view_selected (w->view, &n_sel);
      if (n_sel == 1)
        {
          const GbkEntry *e = gbk_dir_model_get (m, sel[0]);
          g_string_append_printf (s, "\"%s\"", gbk_entry_display (e));
          if (gbk_entry_is_dir (e))
            g_string_append (s, " folder");
          else if (!(e->flags & GBK_ENTRY_STAT_FAILED))
            {
              g_autofree char *size = format_size (e->size);
              g_string_append_printf (s, " (%s)", size);
            }
        }
      else if (n_sel > 1)
        {
          uint64_t total = 0;
          for (size_t i = 0; i < n_sel; i++)
            {
              const GbkEntry *e = gbk_dir_model_get (m, sel[i]);
              if (!gbk_entry_is_dir (e))
                total += e->size;
            }
          g_autofree char *size = format_size (total);
          g_string_append_printf (s, "%zu items selected (%s)", n_sel, size);
        }
      else
        {
          size_t n = gbk_dir_model_n_visible (m), dirs = 0;
          for (size_t i = 0; i < n; i++)
            if (gbk_entry_is_dir (gbk_dir_model_get (m, gbk_dir_model_visible_handle (m, i))))
              dirs++;
          size_t files = n - dirs;
          if (dirs > 0)
            g_string_append_printf (s, "%zu %s", dirs, dirs == 1 ? "folder" : "folders");
          if (files > 0)
            g_string_append_printf (s, "%s%zu %s", dirs > 0 ? ", " : "", files,
                                    files == 1 ? "file" : "files");
          if (n == 0)
            g_string_append (s, gbk_dir_model_state (m) == GBK_DIR_LOADING ? "Loading…" : "Empty");
        }
      uint64_t free_bytes;
      if (gbk_dir_model_free_space (m, &free_bytes))
        {
          g_autofree char *size = format_size (free_bytes);
          g_string_append_printf (s, "  |  Free space: %s", size);
        }
    }
  gtk_label_set_text (w->status, s->str);
}

static void
set_message (GbkWindow *w, const char *message)
{
  g_free (w->message);
  w->message = g_strdup (message);
  update_status (w);
}

/* ---- navigation ---- */

static void
update_title (GbkWindow *w)
{
  g_autofree char *base = g_path_get_basename (w->path);
  g_autofree char *shown = g_filename_display_name (base);
  g_autofree char *title = g_strdup_printf ("%s - gbk-file", shown);
  gtk_window_set_title (GTK_WINDOW (w), title);
}

static void open_path (GbkWindow *w, const char *path, const char *focus_name);

static void
on_model_changed (GbkDirModel *m, GbkWindow *w)
{
  if (m != w->model)
    return;
  if (w->focus_on_load != nullptr)
    {
      GbkHandle h = gbk_dir_model_find (m, w->focus_on_load);
      if (h != GBK_HANDLE_NONE)
        {
          gbk_view_select_only (w->view, h);
          g_clear_pointer (&w->focus_on_load, g_free);
        }
    }
  update_status (w);
}

static void
on_model_state (GbkDirModel *m, GbkWindow *w)
{
  if (m != w->model)
    return;
  GbkDirState state = gbk_dir_model_state (m);
  if (state == GBK_DIR_READY)
    g_clear_pointer (&w->focus_on_load, g_free);   /* it never appeared: forget it */

  /* The directory vanished (deleted, moved, unmounted) or never existed: walk up to the
   * nearest ancestor. Each level is another async attempt, so nothing blocks. */
  bool missing = state == GBK_DIR_GONE
                 || (state == GBK_DIR_ERROR && (gbk_dir_model_error (m) == ENOENT
                                                || gbk_dir_model_error (m) == ENOTDIR));
  if (missing && !g_str_equal (w->path, "/"))
    {
      g_autofree char *parent = g_path_get_dirname (w->path);
      g_autofree char *child = g_path_get_basename (w->path);
      open_path (w, parent, child);
      return;
    }
  update_status (w);
}

static void
set_model (GbkWindow *w, GbkDirModel *m)
{
  if (w->model != nullptr)
    {
      g_signal_handler_disconnect (w->model, w->model_state_id);
      g_signal_handler_disconnect (w->model, w->model_changed_id);
      g_clear_object (&w->model);
    }
  w->model = m;     /* takes the reference */
  if (m != nullptr)
    {
      gbk_dir_model_set_show_hidden (m, w->show_hidden);
      gbk_dir_model_set_sort (m, w->sort);
      w->model_state_id = gbk_connect (G_OBJECT (w), m, "state-changed",
                                       G_CALLBACK (on_model_state), w);
      w->model_changed_id = gbk_connect (G_OBJECT (w), m, "changed",
                                         G_CALLBACK (on_model_changed), w);
    }
  gbk_view_set_model (w->view, m);
}

/* Replace the current directory without touching history. */
static void
open_path (GbkWindow *w, const char *path, const char *focus_name)
{
  g_free (w->path);
  w->path = g_canonicalize_filename (path, nullptr);
  g_free (w->focus_on_load);
  w->focus_on_load = g_strdup (focus_name);
  g_clear_pointer (&w->message, g_free);
  set_model (w, gbk_dir_model_new (w->path, w->ctx->types));
  update_title (w);
  update_status (w);
}

static void
push_history (GPtrArray *stack, const char *path)
{
  if (stack->len >= HISTORY_MAX)
    g_ptr_array_remove_index (stack, 0);
  g_ptr_array_add (stack, g_strdup (path));
}

void
gbk_window_open (GbkWindow *w, const char *path)
{
  if (w->path != nullptr)
    {
      g_autofree char *canon = g_canonicalize_filename (path, nullptr);
      if (g_str_equal (canon, w->path))
        return;
      push_history (w->back, w->path);
      g_ptr_array_set_size (w->forward, 0);
    }
  open_path (w, path, nullptr);
}

static void
go_back (GbkWindow *w)
{
  if (w->back->len == 0)
    return;
  g_autofree char *target = g_ptr_array_steal_index (w->back, w->back->len - 1);
  g_autofree char *came_from = g_path_get_basename (w->path);
  push_history (w->forward, w->path);
  /* If we're going back to the parent we came from, select where we were. */
  g_autofree char *parent = g_path_get_dirname (w->path);
  open_path (w, target, g_str_equal (parent, target) ? came_from : nullptr);
}

static void
go_forward (GbkWindow *w)
{
  if (w->forward->len == 0)
    return;
  g_autofree char *target = g_ptr_array_steal_index (w->forward, w->forward->len - 1);
  push_history (w->back, w->path);
  open_path (w, target, nullptr);
}

static void
go_up (GbkWindow *w)
{
  if (g_str_equal (w->path, "/"))
    return;
  g_autofree char *parent = g_path_get_dirname (w->path);
  g_autofree char *child = g_path_get_basename (w->path);
  push_history (w->back, w->path);
  g_ptr_array_set_size (w->forward, 0);
  open_path (w, parent, child);
}

/* ---- opening things ---- */

/* The selection as owned GbkItems: safe to keep while the model changes underneath. */
static GPtrArray *
snapshot_selection (GbkWindow *w)
{
  GPtrArray *items = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_item_free);
  size_t n;
  g_autofree GbkHandle *sel = gbk_view_selected (w->view, &n);
  for (size_t i = 0; i < n; i++)
    {
      const GbkEntry *e = gbk_dir_model_get (w->model, sel[i]);
      if (e != nullptr)
        g_ptr_array_add (items, gbk_item_new (w->ctx->config, w->ctx->types, w->path, e));
    }
  return items;
}

static void
report (GbkWindow *w, const char *what, const GError *error)
{
  g_autofree char *msg = g_strdup_printf ("%s: %s", what, error->message);
  set_message (w, msg);
}

static GAppLaunchContext *
launch_context (GbkWindow *w)
{
  GdkAppLaunchContext *lc = gdk_display_get_app_launch_context (gtk_widget_get_display (GTK_WIDGET (w)));
  gdk_app_launch_context_set_timestamp (lc, gtk_get_current_event_time ());
  return G_APP_LAUNCH_CONTEXT (lc);
}

/* Double-click / Enter. A single folder is entered; files are run or opened per
 * gbk_open_plan. (Folders in a multi-selection are skipped until there are tabs.) */
static void
open_items (GbkWindow *w, GPtrArray *items)
{
  if (items->len == 1 && ((GbkItem *) g_ptr_array_index (items, 0))->is_dir)
    {
      gbk_window_open (w, ((GbkItem *) g_ptr_array_index (items, 0))->path);
      return;
    }
  g_autoptr (GPtrArray) by_default = g_ptr_array_new ();
  for (guint i = 0; i < items->len; i++)
    {
      GbkItem *it = g_ptr_array_index (items, i);
      g_autoptr (GError) error = nullptr;
      switch (gbk_open_plan (it))
        {
        case GBK_OPEN_ENTER:
          break;
        case GBK_OPEN_RUN:
          if (!gbk_open_execute (w->ctx->config, it, false, w->path, &error))
            report (w, it->display, error);
          break;
        case GBK_OPEN_RUN_TERMINAL:
          if (!gbk_open_execute (w->ctx->config, it, true, w->path, &error))
            report (w, it->display, error);
          break;
        case GBK_OPEN_DEFAULT:
          g_ptr_array_add (by_default, it);
          break;
        default:
          gbk_fatal ("bad open plan");
        }
    }
  if (by_default->len > 0)
    {
      g_autoptr (GAppLaunchContext) lc = launch_context (w);
      g_autoptr (GError) error = nullptr;
      if (!gbk_open_default (by_default, lc, &error))
        report (w, "Open", error);
    }
}

/* ---- view events ---- */

static void
on_activate (GbkView *v G_GNUC_UNUSED, GbkWindow *w)
{
  g_autoptr (GPtrArray) items = snapshot_selection (w);
  open_items (w, items);
}

/* ---- context menu ---- */

/* Each menu item carries its entry kind, plus an action or app; the menu carries the item
 * snapshot. Activations are connected with the window as owner, so nothing fires after the
 * window is gone; the data is owned by the menu (items), so nothing outlives it. */
#define KEY_ITEMS "gbk-items"
#define KEY_KIND "gbk-kind"
#define KEY_ACTION "gbk-action"
#define KEY_APP "gbk-app"

static GPtrArray *
menu_items_of (GtkWidget *menu_item)
{
  GtkWidget *menu = gtk_widget_get_parent (menu_item);
  /* Open With entries live in a submenu: its items are on the top-level menu. */
  while (menu != nullptr && g_object_get_data (G_OBJECT (menu), KEY_ITEMS) == nullptr)
    {
      GtkWidget *attach = GTK_IS_MENU (menu) ? gtk_menu_get_attach_widget (GTK_MENU (menu)) : nullptr;
      menu = attach != nullptr ? gtk_widget_get_parent (attach) : nullptr;
    }
  if (menu == nullptr)
    gbk_fatal ("menu item without a snapshot");
  return g_object_get_data (G_OBJECT (menu), KEY_ITEMS);
}

static void
set_sort (GbkWindow *w, GbkSortMode sort)
{
  w->sort = sort;
  if (w->model != nullptr)
    gbk_dir_model_set_sort (w->model, sort);
}

static void
on_menu_item (GtkMenuItem *mi, GbkWindow *w)
{
  GPtrArray *items = menu_items_of (GTK_WIDGET (mi));
  GbkMenuKind kind = (GbkMenuKind) GPOINTER_TO_INT (g_object_get_data (G_OBJECT (mi), KEY_KIND));
  const GbkItem *first = items->len > 0 ? g_ptr_array_index (items, 0) : nullptr;
  g_autoptr (GError) error = nullptr;
  bool ok = true;

  switch (kind)
    {
    case GBK_MENU_OPEN:
      open_items (w, items);
      break;
    case GBK_MENU_OPEN_WITH:
      {
        GAppInfo *app = g_object_get_data (G_OBJECT (mi), KEY_APP);
        g_autoptr (GAppLaunchContext) lc = launch_context (w);
        ok = gbk_open_with (app, items, lc, &error);
        break;
      }
    case GBK_MENU_EXECUTE:
      ok = gbk_open_execute (w->ctx->config, first, false, w->path, &error);
      break;
    case GBK_MENU_EXECUTE_TERMINAL:
      ok = gbk_open_execute (w->ctx->config, first, true, w->path, &error);
      break;
    case GBK_MENU_TERMINAL_HERE:
      ok = gbk_open_terminal (w->ctx->config, first->path, &error);
      break;
    case GBK_MENU_ACTION:
      ok = gbk_open_action (g_object_get_data (G_OBJECT (mi), KEY_ACTION), items, w->path, &error);
      break;
    case GBK_MENU_SORT_NAME:
      set_sort (w, GBK_SORT_NAME);
      break;
    case GBK_MENU_SORT_MTIME:
      set_sort (w, GBK_SORT_MTIME);
      break;
    case GBK_MENU_SHOW_HIDDEN:
      g_action_group_activate_action (G_ACTION_GROUP (w), "toggle-hidden", nullptr);
      break;
    case GBK_MENU_SEPARATOR:
    default:
      gbk_fatal ("bad menu entry kind %d", (int) kind);
    }
  if (!ok)
    report (w, gtk_menu_item_get_label (mi), error);
}

static GtkWidget *
menu_item (GbkWindow *w, GtkWidget *menu, const char *label, GbkMenuKind kind)
{
  GtkWidget *mi = gtk_menu_item_new_with_label (label);
  g_object_set_data (G_OBJECT (mi), KEY_KIND, GINT_TO_POINTER ((int) kind));
  gbk_connect (G_OBJECT (w), mi, "activate", G_CALLBACK (on_menu_item), w);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
  return mi;
}

static GtkWidget *
check_item (GbkWindow *w, GtkWidget *menu, const char *label, GbkMenuKind kind, bool active,
            bool radio)
{
  GtkWidget *mi = gtk_check_menu_item_new_with_label (label);
  gtk_check_menu_item_set_draw_as_radio (GTK_CHECK_MENU_ITEM (mi), radio);
  gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (mi), active);   /* before connecting */
  g_object_set_data (G_OBJECT (mi), KEY_KIND, GINT_TO_POINTER ((int) kind));
  gbk_connect (G_OBJECT (w), mi, "activate", G_CALLBACK (on_menu_item), w);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
  return mi;
}

static void
add_open_with (GbkWindow *w, GtkWidget *menu, const char *mime)
{
  GList *apps = g_app_info_get_recommended_for_type (mime);
  if (apps == nullptr)
    return;
  GtkWidget *sub = gtk_menu_new ();
  for (GList *l = apps; l != nullptr; l = l->next)
    {
      GAppInfo *app = l->data;
      g_autofree char *label = g_strdup (g_app_info_get_display_name (app));
      GtkWidget *mi = menu_item (w, sub, label, GBK_MENU_OPEN_WITH);
      g_object_set_data_full (G_OBJECT (mi), KEY_APP, g_object_ref (app), g_object_unref);
    }
  g_list_free_full (apps, g_object_unref);
  GtkWidget *parent = gtk_menu_item_new_with_label ("Open With");
  gtk_menu_item_set_submenu (GTK_MENU_ITEM (parent), sub);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), parent);
}

static bool
destroy_menu (void *data)
{
  gtk_widget_destroy (GTK_WIDGET (data));
  return false;
}

static void
on_menu_deactivate (GtkMenuShell *menu, void *data G_GNUC_UNUSED)
{
  /* "deactivate" comes before the chosen item's "activate": destroy on the next idle. */
  gbk_idle_add (G_OBJECT (menu), destroy_menu, menu);
}

static void
on_attach_destroyed (GtkWidget *attach G_GNUC_UNUSED, GtkWidget *menu)
{
  gtk_widget_destroy (menu);
}

static void
on_context_menu (GbkView *v, GdkEvent *event, GbkWindow *w)
{
  g_autoptr (GPtrArray) items = snapshot_selection (w);
  bool background = items->len == 0;
  if (background)
    g_ptr_array_add (items, gbk_item_new_dir (w->path));

  g_autoptr (GArray) entries = gbk_menu_entries (w->ctx->config, items, background);
  GtkWidget *menu = gtk_menu_new ();
  g_object_set_data_full (G_OBJECT (menu), KEY_ITEMS, g_ptr_array_ref (items),
                          (GDestroyNotify) g_ptr_array_unref);

  for (guint i = 0; i < entries->len; i++)
    {
      const GbkMenuEntry *e = &g_array_index (entries, GbkMenuEntry, i);
      switch (e->kind)
        {
        case GBK_MENU_SEPARATOR:
          gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
          break;
        case GBK_MENU_OPEN: menu_item (w, menu, "Open", e->kind); break;
        case GBK_MENU_OPEN_WITH: add_open_with (w, menu, gbk_menu_common_mime (items)); break;
        case GBK_MENU_EXECUTE: menu_item (w, menu, "Execute", e->kind); break;
        case GBK_MENU_EXECUTE_TERMINAL: menu_item (w, menu, "Execute in Terminal", e->kind); break;
        case GBK_MENU_TERMINAL_HERE: menu_item (w, menu, "Open Terminal Here", e->kind); break;
        case GBK_MENU_ACTION:
          {
            GtkWidget *mi = menu_item (w, menu, e->action->name, e->kind);
            g_object_set_data (G_OBJECT (mi), KEY_ACTION, e->action);
            break;
          }
        case GBK_MENU_SORT_NAME:
          check_item (w, menu, "Sort by Name", e->kind, w->sort == GBK_SORT_NAME, true);
          break;
        case GBK_MENU_SORT_MTIME:
          check_item (w, menu, "Sort by Date Modified", e->kind, w->sort == GBK_SORT_MTIME, true);
          break;
        case GBK_MENU_SHOW_HIDDEN:
          check_item (w, menu, "Show Hidden Files", e->kind, w->show_hidden, false);
          break;
        default:
          gbk_fatal ("bad menu entry kind %d", (int) e->kind);
        }
    }

  gtk_menu_attach_to_widget (GTK_MENU (menu), GTK_WIDGET (v), nullptr);
  gbk_connect (G_OBJECT (menu), menu, "deactivate", G_CALLBACK (on_menu_deactivate), nullptr);
  /* A GtkMenu is its own toplevel: destroying the widget it's attached to doesn't destroy
   * it. Tie it to the view, or closing the window with a menu open leaks the menu. */
  gbk_connect (G_OBJECT (menu), v, "destroy", G_CALLBACK (on_attach_destroyed), menu);
  gtk_widget_show_all (menu);
  /* Keyboard-triggered menus come without an event; GTK needs one (for the grab), and the
   * key press being handled is it. */
  g_autoptr (GdkEvent) current = event == nullptr ? gtk_get_current_event () : nullptr;
  GdkEvent *trigger = event != nullptr ? event : current;
  if (trigger != nullptr && trigger->type == GDK_BUTTON_PRESS)
    gtk_menu_popup_at_pointer (GTK_MENU (menu), trigger);
  else
    gtk_menu_popup_at_widget (GTK_MENU (menu), GTK_WIDGET (v), GDK_GRAVITY_CENTER,
                              GDK_GRAVITY_NORTH_WEST, trigger);
}

static void
on_selection_changed (GbkView *v G_GNUC_UNUSED, GbkWindow *w)
{
  g_clear_pointer (&w->message, g_free);
  update_status (w);
}

/* ---- actions ---- */

static void
action_back (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, void *data)
{
  go_back (data);
}

static void
action_forward (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, void *data)
{
  go_forward (data);
}

static void
action_up (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, void *data)
{
  go_up (data);
}

static void
action_toggle_hidden (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, void *data)
{
  GbkWindow *w = data;
  w->show_hidden = !w->show_hidden;
  if (w->model != nullptr)
    gbk_dir_model_set_show_hidden (w->model, w->show_hidden);
}

static void
action_terminal (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, void *data)
{
  GbkWindow *w = data;
  g_autoptr (GError) error = nullptr;
  if (!gbk_open_terminal (w->ctx->config, w->path, &error))
    report (w, "Terminal", error);
}

static void
action_close (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, void *data)
{
  gtk_window_close (GTK_WINDOW (data));
}

static const GActionEntry actions[] = {
  { .name = "back", .activate = action_back },
  { .name = "forward", .activate = action_forward },
  { .name = "up", .activate = action_up },
  { .name = "toggle-hidden", .activate = action_toggle_hidden },
  { .name = "terminal", .activate = action_terminal },
  { .name = "close", .activate = action_close },
};

/* ---- GObject ---- */

static void
gbk_window_dispose (GObject *o)
{
  GbkWindow *w = GBK_WINDOW (o);
  if (w->model != nullptr)
    set_model (w, nullptr);
  G_OBJECT_CLASS (gbk_window_parent_class)->dispose (o);
}

static void
gbk_window_finalize (GObject *o)
{
  GbkWindow *w = GBK_WINDOW (o);
  g_ptr_array_unref (w->back);
  g_ptr_array_unref (w->forward);
  g_free (w->path);
  g_free (w->focus_on_load);
  g_free (w->message);
  gbk_context_unref (w->ctx);
  G_OBJECT_CLASS (gbk_window_parent_class)->finalize (o);
}

static void
gbk_window_class_init (GbkWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = gbk_window_dispose;
  G_OBJECT_CLASS (klass)->finalize = gbk_window_finalize;
}

static void
gbk_window_init (GbkWindow *w)
{
  w->back = g_ptr_array_new_with_free_func (g_free);
  w->forward = g_ptr_array_new_with_free_func (g_free);
}

GbkWindow *
gbk_window_new (GtkApplication *app, GbkContext *ctx, const char *path)
{
  GbkWindow *w = g_object_new (GBK_TYPE_WINDOW, "application", app, nullptr);
  w->ctx = gbk_context_ref (ctx);
  w->show_hidden = ctx->config->show_hidden;
  w->sort = ctx->config->sort;
  g_action_map_add_action_entries (G_ACTION_MAP (w), actions, G_N_ELEMENTS (actions), w);
  gtk_window_set_default_size (GTK_WINDOW (w), 700, 440);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroller = gtk_scrolled_window_new (nullptr, nullptr);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_AUTOMATIC,
                                  GTK_POLICY_NEVER);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroller), GTK_SHADOW_IN);
  w->view = GBK_VIEW (gbk_view_new (ctx->types));
  gtk_container_add (GTK_CONTAINER (scroller), GTK_WIDGET (w->view));
  gtk_box_pack_start (GTK_BOX (box), scroller, true, true, 0);

  w->status = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_xalign (w->status, 0);
  gtk_label_set_ellipsize (w->status, PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_start (GTK_WIDGET (w->status), 6);
  gtk_widget_set_margin_end (GTK_WIDGET (w->status), 6);
  gtk_widget_set_margin_top (GTK_WIDGET (w->status), 2);
  gtk_widget_set_margin_bottom (GTK_WIDGET (w->status), 2);
  gtk_box_pack_start (GTK_BOX (box), GTK_WIDGET (w->status), false, false, 0);
  gtk_container_add (GTK_CONTAINER (w), box);

  gbk_connect (G_OBJECT (w), w->view, "activate", G_CALLBACK (on_activate), w);
  gbk_connect (G_OBJECT (w), w->view, "selection-changed", G_CALLBACK (on_selection_changed), w);
  gbk_connect (G_OBJECT (w), w->view, "search-changed", G_CALLBACK (on_selection_changed), w);
  gbk_connect (G_OBJECT (w), w->view, "context-menu", G_CALLBACK (on_context_menu), w);

  gbk_window_open (w, path);
  gtk_widget_grab_focus (GTK_WIDGET (w->view));
  return w;
}

const char *
gbk_window_path (GbkWindow *w)
{
  return w->path;
}

GbkView *
gbk_window_view (GbkWindow *w)
{
  return w->view;
}

const char *
gbk_window_status (GbkWindow *w)
{
  return gtk_label_get_text (w->status);
}
