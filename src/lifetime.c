#include "lifetime.h"

static int live_count;

int
gbk_lifetime_live (void)
{
  return g_atomic_int_get (&live_count);
}

/* ---- Signals ----
 * A GClosure watched by the owner (g_object_watch_closure) is invalidated when the owner is
 * disposed; GSignal removes a handler whose closure is invalidated. The closure is also
 * dropped when the instance is finalized. Either way it is finalized exactly once, which is
 * where the live count goes down. The watch also holds a ref on the owner while the
 * callback runs, so the owner cannot be freed mid-callback. */

static void
closure_finalized (void *data G_GNUC_UNUSED, GClosure *closure G_GNUC_UNUSED)
{
  g_atomic_int_dec_and_test (&live_count);
}

static gulong
connect_impl (GObject *owner, gpointer instance, const char *signal,
              GCallback callback, void *data, bool after)
{
  g_assert (G_IS_OBJECT (owner));
  g_assert (G_IS_OBJECT (instance));
  g_assert (signal != nullptr);
  g_assert (callback != nullptr);

  GClosure *closure = g_cclosure_new (callback, data, nullptr);
  g_closure_add_finalize_notifier (closure, nullptr, closure_finalized);
  g_atomic_int_inc (&live_count);
  g_object_watch_closure (owner, closure);

  gulong id = g_signal_connect_closure (instance, signal, closure, after);
  if (id == 0)
    gbk_fatal ("gbk_connect: no signal \"%s\" on %s", signal, G_OBJECT_TYPE_NAME (instance));
  return id;
}

gulong
gbk_connect (GObject *owner, gpointer instance, const char *signal,
             GCallback callback, void *data)
{
  return connect_impl (owner, instance, signal, callback, data, false);
}

gulong
gbk_connect_after (GObject *owner, gpointer instance, const char *signal,
                   GCallback callback, void *data)
{
  return connect_impl (owner, instance, signal, callback, data, true);
}

/* ---- Sources ----
 * The record is freed only by the source's destroy notify, which GLib defers until any
 * in-progress dispatch has returned, so `r` is valid for the whole of source_dispatch. */

typedef struct
{
  GObject *owner;      /* nullptr once the owner has been disposed */
  GbkSourceFunc fn;    /* exactly one of fn / fd_fn is set */
  GbkFdFunc fd_fn;
  void *data;
  GSource *source;     /* borrowed; valid until source_destroyed */
} SourceRec;

static void
owner_disposed (void *p, GObject *where_the_object_was G_GNUC_UNUSED)
{
  SourceRec *r = p;
  r->owner = nullptr;            /* the weak ref is already gone; don't unref it later */
  g_source_destroy (r->source);
}

static gboolean
dispatch_common (SourceRec *r, int fd, GIOCondition condition)
{
  g_assert (r->owner != nullptr);  /* a destroyed source is never dispatched */

  /* Hold the owner across the callback: if the callback drops the last other ref, the
   * owner is disposed at our unref below, after the callback has returned. */
  GObject *owner = g_object_ref (r->owner);
  bool keep = r->fn != nullptr ? r->fn (r->data) : r->fd_fn (fd, condition, r->data);
  g_object_unref (owner);

  return (keep && r->owner != nullptr) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean
source_dispatch (void *p)
{
  return dispatch_common (p, -1, 0);
}

static gboolean
fd_source_dispatch (int fd, GIOCondition condition, void *p)
{
  /* G_IO_NVAL: the fd was closed while still watched. Its number may already belong to
   * something else, so carrying on would read the wrong file. Always a lifetime bug. */
  if (condition & G_IO_NVAL)
    gbk_fatal ("fd watch on closed fd %d", fd);
  return dispatch_common (p, fd, condition);
}

static void
source_destroyed (void *p)
{
  SourceRec *r = p;
  if (r->owner != nullptr)
    g_object_weak_unref (r->owner, owner_disposed, r);
  g_free (r);
  g_atomic_int_dec_and_test (&live_count);
}

static guint
add_source (GObject *owner, GSource *source, GbkSourceFunc fn, GbkFdFunc fd_fn, void *data)
{
  g_assert (G_IS_OBJECT (owner));
  g_assert ((fn != nullptr) != (fd_fn != nullptr));

  SourceRec *r = g_new0 (SourceRec, 1);
  r->owner = owner;
  r->fn = fn;
  r->fd_fn = fd_fn;
  r->data = data;
  r->source = source;
  g_atomic_int_inc (&live_count);

  g_object_weak_ref (owner, owner_disposed, r);
  /* A unix fd source calls its callback as GUnixFDSourceFunc; G_SOURCE_FUNC is GLib's
   * sanctioned cast for registering it. */
  g_source_set_callback (source,
                         fd_fn != nullptr ? G_SOURCE_FUNC (fd_source_dispatch) : source_dispatch,
                         r, source_destroyed);
  guint id = g_source_attach (source, nullptr);
  g_source_unref (source);         /* the main context holds it now */
  return id;
}

guint
gbk_idle_add (GObject *owner, GbkSourceFunc fn, void *data)
{
  return add_source (owner, g_idle_source_new (), fn, nullptr, data);
}

guint
gbk_timeout_add (GObject *owner, guint interval_ms, GbkSourceFunc fn, void *data)
{
  return add_source (owner, g_timeout_source_new (interval_ms), fn, nullptr, data);
}

guint
gbk_fd_watch (GObject *owner, int fd, GIOCondition condition, GbkFdFunc fn, void *data)
{
  g_assert (fd >= 0);
  g_assert (fn != nullptr);
  return add_source (owner, g_unix_fd_source_new (fd, condition), nullptr, fn, data);
}

/* ---- Clipboard requests ----
 * GTK always calls the callback eventually (with empty data on failure), but possibly after
 * the requester is gone. A weak ref decides whether `fn` may run. */

typedef struct
{
  GWeakRef owner;
  GbkClipboardFunc fn;
  void *data;
  GDestroyNotify destroy;
} ClipReq;

static void
clipboard_received (GtkClipboard *clipboard G_GNUC_UNUSED, GtkSelectionData *sd, void *p)
{
  ClipReq *r = p;
  GObject *owner = g_weak_ref_get (&r->owner);
  if (owner != nullptr)
    {
      r->fn (sd, r->data);
      g_object_unref (owner);
    }
  if (r->destroy != nullptr)
    r->destroy (r->data);
  g_weak_ref_clear (&r->owner);
  g_free (r);
  g_atomic_int_dec_and_test (&live_count);
}

void
gbk_clipboard_request (GObject *owner, GtkClipboard *clipboard, GdkAtom target,
                       GbkClipboardFunc fn, void *data, GDestroyNotify destroy)
{
  g_assert (G_IS_OBJECT (owner) && fn != nullptr);
  ClipReq *r = g_new0 (ClipReq, 1);
  g_weak_ref_init (&r->owner, owner);
  r->fn = fn;
  r->data = data;
  r->destroy = destroy;
  g_atomic_int_inc (&live_count);
  gtk_clipboard_request_contents (clipboard, target, clipboard_received, r);
}

void
gbk_source_cancel (guint *id)
{
  g_assert (id != nullptr);
  if (*id == 0)
    return;
  /* Look it up rather than calling g_source_remove() blindly: a source that already
   * returned false is gone, and removing a stale id is a GLib critical. */
  GSource *source = g_main_context_find_source_by_id (nullptr, *id);
  if (source != nullptr)
    g_source_destroy (source);
  *id = 0;
}
