/* Location bar: one button per folder from home (or the filesystem root) to the current
 * one, which is shown pressed. Going up keeps the deeper buttons, so you can go back down
 * with one click (as Thunar does). The pencil button (or Ctrl+L) swaps the buttons for a
 * text entry; Enter goes there, Escape goes back to buttons. */
#pragma once

#define GBK_TYPE_PATHBAR (gbk_pathbar_get_type ())
G_DECLARE_FINAL_TYPE (GbkPathbar, gbk_pathbar, GBK, PATHBAR, GtkBox)

/* Signals:
 *   "navigate" (const char *path)   a button was clicked or a location was entered */

GtkWidget *gbk_pathbar_new (void);
void gbk_pathbar_set_path (GbkPathbar *bar, const char *path);
/* Like set_path, but drop the remembered deeper buttons (e.g. another tab came forward). */
void gbk_pathbar_reset (GbkPathbar *bar, const char *path);
/* Switch to the text entry, focused, with the current path selected. */
void gbk_pathbar_edit (GbkPathbar *bar);
bool gbk_pathbar_editing (GbkPathbar *bar);

/* For tests: the button labels, "|"-separated, with the pressed one in [brackets]. */
char *gbk_pathbar_describe (GbkPathbar *bar);
/* For tests: click the button for `path`. */
void gbk_pathbar_click (GbkPathbar *bar, const char *path);
GtkEntry *gbk_pathbar_entry (GbkPathbar *bar);
