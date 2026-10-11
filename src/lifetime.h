/* Owner-scoped signal connections and main-loop sources.
 *
 * Every callback registered here has an owner GObject. When the owner is disposed
 * (gtk_widget_destroy, or its last unref), every callback it owns is disconnected or
 * removed before the owner's memory can go away, so a callback can never run with a
 * dangling owner/data pointer. Signal connections also end when the emitting instance
 * is finalized.
 *
 * The raw GLib/GTK equivalents are poisoned (prelude.h); this is the only way in.
 * Main thread only. */
#pragma once

/* Return true to keep the source, false to remove it. */
typedef bool (*GbkSourceFunc)(void *data);
typedef bool (*GbkFdFunc)(int fd, GIOCondition condition, void *data);

/* Connect `callback` to `signal` on `instance`. `data` is passed as the handler's user_data.
 * Returns the handler id (never 0; an invalid signal name is a fatal programming error). */
gulong gbk_connect(GObject *owner, gpointer instance, const char *signal,
                   GCallback callback, void *data);
gulong gbk_connect_after(GObject *owner, gpointer instance, const char *signal,
                         GCallback callback, void *data);

/* Run `fn(data)` when idle / every `interval_ms`, until it returns false or `owner` is
 * disposed. Returns a source id usable with gbk_source_cancel(). */
guint gbk_idle_add(GObject *owner, GbkSourceFunc fn, void *data);
guint gbk_timeout_add(GObject *owner, guint interval_ms, GbkSourceFunc fn, void *data);

/* Call `fn` whenever `fd` meets `condition`, until it returns false or `owner` is disposed.
 * Does not take ownership of `fd`, which must stay open while the watch exists. The watch
 * is removed when the owner's dispose chains up to GObject's, so a subclass dispose that
 * closes the fd *before* chaining up must gbk_source_cancel() the watch first. Closing it
 * in finalize is always safe. */
guint gbk_fd_watch(GObject *owner, int fd, GIOCondition condition, GbkFdFunc fn, void *data);

/* Request clipboard contents for `target`. `fn` runs only if `owner` is still alive when
 * the (asynchronous) answer arrives. `destroy` (may be nullptr) is always called on `data`
 * afterwards, whether or not `fn` ran. `sd` may hold no data (empty / unsupported). */
typedef void (*GbkClipboardFunc) (GtkSelectionData *sd, void *data);
void gbk_clipboard_request (GObject *owner, GtkClipboard *clipboard, GdkAtom target,
                            GbkClipboardFunc fn, void *data, GDestroyNotify destroy);

/* Cancel a source early and zero `*id`. Safe if the source already finished or `*id` is 0. */
void gbk_source_cancel(guint *id);

/* Number of live registrations (connections + sources). Tests assert this returns to 0. */
int gbk_lifetime_live(void);
