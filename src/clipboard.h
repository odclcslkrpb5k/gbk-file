/* The file clipboard (Ctrl+C / Ctrl+X / Ctrl+V), interoperable with other file managers. */
#pragma once

/* Put absolute `paths` on the CLIPBOARD as a copy or a cut. */
void gbk_clipboard_set_files (GtkWidget *widget, GPtrArray *paths, bool cut);

/* Put plain text on the CLIPBOARD (replacing any files we put there). */
void gbk_clipboard_set_text (GtkWidget *widget, const char *text);

/* Read files from the CLIPBOARD (asynchronously). `fn` runs only if `owner` is still alive;
 * `paths` is empty if the clipboard holds no files. */
typedef void (*GbkFilesFunc) (GPtrArray *paths, bool cut, void *data);
void gbk_clipboard_get_files (GObject *owner, GtkWidget *widget, GbkFilesFunc fn, void *data);

/* After a cut has been pasted: clear the clipboard if we still own it. */
void gbk_clipboard_clear_if_ours (GtkWidget *widget);
