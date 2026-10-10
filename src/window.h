/* A browser window: the compact view of one directory, history, and a status bar. */
#pragma once

#include "context.h"
#include "view.h"

#define GBK_TYPE_WINDOW (gbk_window_get_type ())
G_DECLARE_FINAL_TYPE (GbkWindow, gbk_window, GBK, WINDOW, GtkApplicationWindow)

GbkWindow *gbk_window_new (GtkApplication *app, GbkContext *ctx, const char *path);

/* Navigate to `path` (absolute), recording history. */
void gbk_window_open (GbkWindow *w, const char *path);
const char *gbk_window_path (GbkWindow *w);
GbkView *gbk_window_view (GbkWindow *w);
/* The status bar text, for tests. */
const char *gbk_window_status (GbkWindow *w);
