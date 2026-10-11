#include "pathbar.h"
#include "lifetime.h"

#define KEY_PATH "gbk-path"

struct _GbkPathbar
{
  GtkBox parent_instance;
  GtkStack *stack;
  GtkBox *buttons;
  GtkScrolledWindow *scroller;
  GtkEntry *entry;
  GtkToggleButton *edit;
  char *path;          /* current */
  char *deep;          /* deepest path whose buttons are shown */
  bool updating;       /* programmatic toggles: ignore their "clicked" */
};

G_DEFINE_FINAL_TYPE (GbkPathbar, gbk_pathbar, GTK_TYPE_BOX)

enum { SIGNAL_NAVIGATE, N_SIGNALS };
static guint signals[N_SIGNALS];

static bool
is_prefix_dir (const char *prefix, const char *path)
{
  size_t n = strlen (prefix);
  return strncmp (prefix, path, n) == 0
         && (path[n] == '\0' || path[n] == '/' || g_str_equal (prefix, "/"));
}

static void
on_button_clicked (GtkToggleButton *button, GbkPathbar *bar)
{
  if (bar->updating)
    return;
  const char *target = g_object_get_data (G_OBJECT (button), KEY_PATH);
  /* Keep the pressed state honest until the window confirms the navigation. */
  bar->updating = true;
  gtk_toggle_button_set_active (button, g_str_equal (target, bar->path));
  bar->updating = false;
  g_autofree char *copy = g_strdup (target);
  g_signal_emit (bar, signals[SIGNAL_NAVIGATE], 0, copy);
}

static GtkWidget *
make_button (GbkPathbar *bar, const char *path, const char *label, const char *icon)
{
  GtkWidget *button = gtk_toggle_button_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  if (icon != nullptr)
    gtk_box_pack_start (GTK_BOX (box), gtk_image_new_from_icon_name (icon, GTK_ICON_SIZE_MENU),
                        false, false, 0);
  GtkWidget *l = gtk_label_new (label);
  gtk_box_pack_start (GTK_BOX (box), l, false, false, 0);
  gtk_container_add (GTK_CONTAINER (button), box);
  gtk_widget_set_focus_on_click (button, false);
  g_object_set_data_full (G_OBJECT (button), KEY_PATH, g_strdup (path), g_free);
  g_object_set_data_full (G_OBJECT (button), "gbk-label", g_strdup (label), g_free);
  gbk_connect (G_OBJECT (bar), button, "clicked", G_CALLBACK (on_button_clicked), bar);
  return button;
}

static void
rebuild (GbkPathbar *bar)
{
  g_autoptr (GList) old = gtk_container_get_children (GTK_CONTAINER (bar->buttons));
  for (GList *l = old; l != nullptr; l = l->next)
    gtk_widget_destroy (l->data);

  const char *home = g_get_home_dir ();
  bool in_home = home != nullptr && !g_str_equal (home, "/") && is_prefix_dir (home, bar->deep);
  const char *start = in_home ? home : "/";
  g_autofree char *start_label = in_home ? g_filename_display_basename (home)
                                         : g_strdup ("File System");
  gtk_box_pack_start (bar->buttons,
                      make_button (bar, start, start_label,
                                   in_home ? "user-home" : "drive-harddisk"),
                      false, false, 0);

  /* One button per component below the start. */
  const char *rest = bar->deep + strlen (start);
  g_auto (GStrv) parts = g_strsplit (rest, "/", -1);
  g_autofree char *acc = g_strdup (g_str_equal (start, "/") ? "" : start);
  for (size_t i = 0; parts[i] != nullptr; i++)
    {
      if (parts[i][0] == '\0')
        continue;
      char *next = g_strconcat (acc, "/", parts[i], nullptr);
      g_free (acc);
      acc = next;
      g_autofree char *label = g_filename_display_name (parts[i]);
      gtk_box_pack_start (bar->buttons, make_button (bar, acc, label, nullptr), false, false, 0);
    }
  gtk_widget_show_all (GTK_WIDGET (bar->buttons));
}

static bool
scroll_to_current (void *data)
{
  GbkPathbar *bar = data;
  g_autoptr (GList) kids = gtk_container_get_children (GTK_CONTAINER (bar->buttons));
  for (GList *l = kids; l != nullptr; l = l->next)
    if (gtk_toggle_button_get_active (l->data))
      {
        GtkAllocation a;
        gtk_widget_get_allocation (l->data, &a);
        GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment (bar->scroller);
        double page = gtk_adjustment_get_page_size (adj);
        gtk_adjustment_clamp_page (adj, a.x, MIN (a.x + a.width, a.x + page));
      }
  return false;
}

void
gbk_pathbar_set_path (GbkPathbar *bar, const char *path)
{
  g_free (bar->path);
  bar->path = g_strdup (path);
  if (bar->deep == nullptr || !is_prefix_dir (path, bar->deep))
    {
      g_free (bar->deep);
      bar->deep = g_strdup (path);
      rebuild (bar);
    }
  bar->updating = true;
  g_autoptr (GList) kids = gtk_container_get_children (GTK_CONTAINER (bar->buttons));
  for (GList *l = kids; l != nullptr; l = l->next)
    gtk_toggle_button_set_active (l->data,
                                  g_str_equal (g_object_get_data (l->data, KEY_PATH), path));
  bar->updating = false;
  /* After layout: bring the pressed button into view (long paths scroll). */
  gbk_idle_add (G_OBJECT (bar), scroll_to_current, bar);
}

/* ---- text entry mode ---- */

static void
show_buttons (GbkPathbar *bar)
{
  gtk_stack_set_visible_child_name (bar->stack, "buttons");
  bar->updating = true;
  gtk_toggle_button_set_active (bar->edit, false);
  bar->updating = false;
}

void
gbk_pathbar_edit (GbkPathbar *bar)
{
  g_autofree char *shown = g_filename_display_name (bar->path != nullptr ? bar->path : "/");
  gtk_entry_set_text (bar->entry, shown);
  gtk_stack_set_visible_child_name (bar->stack, "entry");
  bar->updating = true;
  gtk_toggle_button_set_active (bar->edit, true);
  bar->updating = false;
  gtk_widget_grab_focus (GTK_WIDGET (bar->entry));
  gtk_editable_select_region (GTK_EDITABLE (bar->entry), 0, -1);
}

bool
gbk_pathbar_editing (GbkPathbar *bar)
{
  return g_str_equal (gtk_stack_get_visible_child_name (bar->stack), "entry");
}

static void
on_entry_activate (GtkEntry *entry, GbkPathbar *bar)
{
  const char *text = gtk_entry_get_text (entry);
  g_autofree char *expanded = nullptr;
  if (text[0] == '~' && (text[1] == '\0' || text[1] == '/'))
    expanded = g_strconcat (g_get_home_dir (), text + 1, nullptr);
  else
    expanded = g_strdup (text);
  g_autofree char *fs = g_filename_from_utf8 (expanded, -1, nullptr, nullptr, nullptr);
  show_buttons (bar);
  if (fs == nullptr || fs[0] == '\0')
    return;
  /* Relative input is relative to the current folder. */
  g_autofree char *abs = g_canonicalize_filename (fs, bar->path);
  g_signal_emit (bar, signals[SIGNAL_NAVIGATE], 0, abs);
}

static gboolean
on_entry_key (GtkWidget *w G_GNUC_UNUSED, GdkEventKey *ev, GbkPathbar *bar)
{
  if (ev->keyval == GDK_KEY_Escape)
    {
      show_buttons (bar);
      return true;
    }
  return false;
}

static void
on_edit_toggled (GtkToggleButton *b, GbkPathbar *bar)
{
  if (bar->updating)
    return;
  if (gtk_toggle_button_get_active (b))
    gbk_pathbar_edit (bar);
  else
    show_buttons (bar);
}

/* ---- tests ---- */

char *
gbk_pathbar_describe (GbkPathbar *bar)
{
  GString *s = g_string_new ("");
  g_autoptr (GList) kids = gtk_container_get_children (GTK_CONTAINER (bar->buttons));
  for (GList *l = kids; l != nullptr; l = l->next)
    {
      const char *label = g_object_get_data (l->data, "gbk-label");
      if (s->len > 0)
        g_string_append_c (s, '|');
      if (gtk_toggle_button_get_active (l->data))
        g_string_append_printf (s, "[%s]", label);
      else
        g_string_append (s, label);
    }
  return g_string_free (s, false);
}

void
gbk_pathbar_click (GbkPathbar *bar, const char *path)
{
  g_autoptr (GList) kids = gtk_container_get_children (GTK_CONTAINER (bar->buttons));
  for (GList *l = kids; l != nullptr; l = l->next)
    if (g_str_equal (g_object_get_data (l->data, KEY_PATH), path))
      {
        gtk_button_clicked (GTK_BUTTON (l->data));
        return;
      }
  gbk_fatal ("no path bar button for %s", path);
}

GtkEntry *
gbk_pathbar_entry (GbkPathbar *bar)
{
  return bar->entry;
}

/* ---- GObject ---- */

static void
gbk_pathbar_finalize (GObject *o)
{
  GbkPathbar *bar = GBK_PATHBAR (o);
  g_free (bar->path);
  g_free (bar->deep);
  G_OBJECT_CLASS (gbk_pathbar_parent_class)->finalize (o);
}

static void
gbk_pathbar_class_init (GbkPathbarClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = gbk_pathbar_finalize;
  signals[SIGNAL_NAVIGATE] = g_signal_new ("navigate", G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
                                           G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gbk_pathbar_init (GbkPathbar *bar)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (bar), GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing (GTK_BOX (bar), 4);

  bar->stack = GTK_STACK (gtk_stack_new ());
  bar->scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new (nullptr, nullptr));
  /* EXTERNAL: scrollable (wheel, programmatic) without a visible scrollbar. */
  gtk_scrolled_window_set_policy (bar->scroller, GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  bar->buttons = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (bar->buttons)),
                               GTK_STYLE_CLASS_LINKED);
  gtk_container_add (GTK_CONTAINER (bar->scroller), GTK_WIDGET (bar->buttons));
  bar->entry = GTK_ENTRY (gtk_entry_new ());
  gtk_stack_add_named (bar->stack, GTK_WIDGET (bar->scroller), "buttons");
  gtk_stack_add_named (bar->stack, GTK_WIDGET (bar->entry), "entry");
  gtk_box_pack_start (GTK_BOX (bar), GTK_WIDGET (bar->stack), true, true, 0);

  bar->edit = GTK_TOGGLE_BUTTON (gtk_toggle_button_new ());
  gtk_container_add (GTK_CONTAINER (bar->edit),
                     gtk_image_new_from_icon_name ("document-edit-symbolic", GTK_ICON_SIZE_MENU));
  gtk_widget_set_tooltip_text (GTK_WIDGET (bar->edit), "Type a location (Ctrl+L)");
  gtk_widget_set_focus_on_click (GTK_WIDGET (bar->edit), false);
  gtk_box_pack_start (GTK_BOX (bar), GTK_WIDGET (bar->edit), false, false, 0);

  gbk_connect (G_OBJECT (bar), bar->entry, "activate", G_CALLBACK (on_entry_activate), bar);
  gbk_connect (G_OBJECT (bar), bar->entry, "key-press-event", G_CALLBACK (on_entry_key), bar);
  gbk_connect (G_OBJECT (bar), bar->edit, "toggled", G_CALLBACK (on_edit_toggled), bar);
}

GtkWidget *
gbk_pathbar_new (void)
{
  return g_object_new (GBK_TYPE_PATHBAR, nullptr);
}
