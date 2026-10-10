#include "window.h"
#include "launch.h"
#include "lifetime.h"

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

/* ---- view events ---- */

static void
on_activate (GbkView *v, GbkWindow *w)
{
  size_t n;
  g_autofree GbkHandle *sel = gbk_view_selected (v, &n);
  if (n != 1)
    return;   /* TODO(launch): open files with their default handler */
  const GbkEntry *e = gbk_dir_model_get (w->model, sel[0]);
  if (e == nullptr || !gbk_entry_is_dir (e))
    return;   /* TODO(launch) */
  g_autofree char *target = g_build_filename (w->path, gbk_entry_name (e), nullptr);
  gbk_window_open (w, target);
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
  GbkCommandArgs args = { .dir = w->path };
  g_autoptr (GError) error = nullptr;
  if (!gbk_launch_command (w->ctx->config->terminal, &args, &error))
    {
      g_autofree char *msg = g_strdup_printf ("Terminal: %s", error->message);
      set_message (w, msg);
    }
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
