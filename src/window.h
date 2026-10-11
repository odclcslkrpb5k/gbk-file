/* A browser window: path bar, tabs (one GbkBrowser each), status bar. Actions apply to the
 * active tab; show-hidden and sort apply window-wide. */
#pragma once

#include "browser.h"
#include "pathbar.h"

#define GBK_TYPE_WINDOW (gbk_window_get_type ())
G_DECLARE_FINAL_TYPE (GbkWindow, gbk_window, GBK, WINDOW, GtkApplicationWindow)

GbkWindow *gbk_window_new (GtkApplication *app, GbkContext *ctx, const char *path);
/* Keyboard shortcuts for every window of `app`. Call once. */
void gbk_window_install_accels (GtkApplication *app);

/* Navigate the active tab to `path` (absolute), recording history. */
void gbk_window_open (GbkWindow *w, const char *path);
void gbk_window_new_tab (GbkWindow *w, const char *path);
const char *gbk_window_path (GbkWindow *w);
GbkView *gbk_window_view (GbkWindow *w);
const char *gbk_window_status (GbkWindow *w);
/* Right side of the status bar: "Sort: … | Hidden files: …". */
const char *gbk_window_modes (GbkWindow *w);
int gbk_window_n_tabs (GbkWindow *w);
GbkPathbar *gbk_window_pathbar (GbkWindow *w);
