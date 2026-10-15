/* A browser window: tabs (one GbkBrowser each, with its path bar), status bar. Actions apply to the
 * active tab; show-hidden and sort apply window-wide. */
#pragma once

#include "browser.h"
#include "devices.h"
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
/* Right side of the status bar: the sort button (label "↑Name" etc.; click cycles name ->
 * modified -> size) and the ".*" hidden-files toggle. */
GtkButton *gbk_window_sort_button (GbkWindow *w);
/* The whole status bar (right-click: the folder's menu, as on empty space). */
GtkWidget *gbk_window_status_bar (GbkWindow *w);
GtkToggleButton *gbk_window_hidden_button (GbkWindow *w);
int gbk_window_n_tabs (GbkWindow *w);
const char *gbk_window_tab_path (GbkWindow *w, int i);   /* tab `i`'s folder */
GbkPathbar *gbk_window_pathbar (GbkWindow *w);
/* The ☰ menu's Devices list. It starts following the system this long after the window
 * opens (the first folder comes first). */
GbkDevices *gbk_window_devices (GbkWindow *w);
#define GBK_WINDOW_DEVICES_DELAY_MS 300
