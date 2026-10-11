#include "inotify-hub.h"

typedef struct
{
  int wd;
  uint32_t mask;
  char *name;           /* owned; nullptr for events about the watched inode itself */
} Event;

struct GbkInotifySub
{
  GMutex lock;          /* guards events, overflow */
  GArray *events;       /* Event */
  bool overflow;
  int efd;              /* eventfd: readable when events are pending */
  GArray *wds;          /* int: watches this subscriber added (owned by the hub lock) */
};

/* Hub state, all under hub_lock. */
static GMutex hub_lock;
static int hub_fd = -1;
static GHashTable *wd_subs;      /* wd -> GPtrArray of GbkInotifySub* (not owned) */
static int max_pending = GBK_INOTIFY_MAX_PENDING;   /* atomic */

void
gbk_inotify_set_max_pending_for_test (unsigned n)
{
  g_atomic_int_set (&max_pending, (int) MIN (n, (unsigned) INT_MAX));
}

static void
event_clear (void *p)
{
  Event *e = p;
  g_free (e->name);
}

static void
eventfd_signal (int fd)
{
  uint64_t one = 1;
  while (write (fd, &one, sizeof one) < 0 && errno == EINTR)
    ;
}

/* ---- the hub thread ---- */

static void
deliver (GbkInotifySub *sub, int wd, uint32_t mask, const char *name, size_t name_len)
{
  g_mutex_lock (&sub->lock);
  bool was_empty = sub->events->len == 0 && !sub->overflow;
  if (sub->events->len >= (guint) g_atomic_int_get (&max_pending))
    sub->overflow = true;           /* subscriber can't keep up: it will rescan */
  else
    {
      Event e = { wd, mask, name != nullptr ? g_strndup (name, name_len) : nullptr };
      g_array_append_val (sub->events, e);
    }
  g_mutex_unlock (&sub->lock);
  if (was_empty)
    eventfd_signal (sub->efd);
}

static void
dispatch (int wd, uint32_t mask, const char *name, size_t name_len, void *data G_GNUC_UNUSED)
{
  /* Called with hub_lock held. */
  if (mask & IN_Q_OVERFLOW)
    {
      /* The kernel queue overflowed: everyone may have lost events. */
      GHashTableIter it;
      void *subs;
      g_hash_table_iter_init (&it, wd_subs);
      while (g_hash_table_iter_next (&it, nullptr, &subs))
        for (guint i = 0; i < ((GPtrArray *) subs)->len; i++)
          deliver (g_ptr_array_index ((GPtrArray *) subs, i), -1, IN_Q_OVERFLOW, nullptr, 0);
      return;
    }
  GPtrArray *subs = g_hash_table_lookup (wd_subs, GINT_TO_POINTER (wd));
  if (subs == nullptr)
    return;                         /* watch already removed: stragglers */
  for (guint i = 0; i < subs->len; i++)
    deliver (g_ptr_array_index (subs, i), wd, mask, name, name_len);
  if (mask & IN_IGNORED)
    g_hash_table_remove (wd_subs, GINT_TO_POINTER (wd));   /* the kernel dropped it */
}

static void *
hub_main (void *data G_GNUC_UNUSED)
{
  alignas (struct inotify_event) uint8_t buf[65536];
  for (;;)
    {
      ssize_t n = read (hub_fd, buf, sizeof buf);
      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          gbk_fatal ("inotify hub read: %s", g_strerror (errno));
        }
      g_mutex_lock (&hub_lock);
      if (!gbk_inotify_parse (buf, (size_t) n, dispatch, nullptr))
        dispatch (-1, IN_Q_OVERFLOW, nullptr, 0, nullptr);   /* garbled: everyone rescans */
      g_mutex_unlock (&hub_lock);
    }
  return nullptr;
}

/* Lazily start the hub. Returns false if inotify is unavailable (e.g. instance limit). */
static bool
hub_ensure (void)
{
  /* hub_lock held. */
  if (hub_fd >= 0)
    return true;
  int fd = inotify_init1 (IN_CLOEXEC);
  if (fd < 0)
    return false;
  g_autoptr (GError) error = nullptr;
  hub_fd = fd;
  wd_subs = g_hash_table_new_full (g_direct_hash, g_direct_equal, nullptr,
                                   (GDestroyNotify) g_ptr_array_unref);
  GThread *t = g_thread_try_new ("gbk-inotify", hub_main, nullptr, &error);
  if (t == nullptr)
    {
      close (hub_fd);
      hub_fd = -1;
      g_clear_pointer (&wd_subs, g_hash_table_unref);
      return false;
    }
  g_thread_unref (t);   /* lives for the process */
  return true;
}

/* ---- subscribers ---- */

GbkInotifySub *
gbk_inotify_sub_new (void)
{
  GbkInotifySub *sub = g_new0 (GbkInotifySub, 1);
  g_mutex_init (&sub->lock);
  sub->events = g_array_new (false, false, sizeof (Event));
  g_array_set_clear_func (sub->events, event_clear);
  sub->wds = g_array_new (false, false, sizeof (int));
  sub->efd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (sub->efd < 0)
    gbk_fatal ("eventfd: %s", g_strerror (errno));
  return sub;
}

int
gbk_inotify_sub_fd (const GbkInotifySub *sub)
{
  return sub->efd;
}

/* hub_lock held. */
static int
watch_locked (GbkInotifySub *sub, const char *path, uint32_t mask)
{
  if (!hub_ensure ())
    return -1;
  int wd = inotify_add_watch (hub_fd, path, mask | IN_MASK_ADD);
  if (wd < 0)
    return -1;
  GPtrArray *subs = g_hash_table_lookup (wd_subs, GINT_TO_POINTER (wd));
  if (subs == nullptr)
    {
      subs = g_ptr_array_new ();
      g_hash_table_insert (wd_subs, GINT_TO_POINTER (wd), subs);
    }
  if (!g_ptr_array_find (subs, sub, nullptr))
    {
      g_ptr_array_add (subs, sub);
      g_array_append_val (sub->wds, wd);
    }
  return wd;
}

int
gbk_inotify_sub_watch (GbkInotifySub *sub, const char *path, uint32_t mask)
{
  g_mutex_lock (&hub_lock);
  int wd = watch_locked (sub, path, mask);
  int saved = errno;
  g_mutex_unlock (&hub_lock);
  errno = saved;
  return wd;
}

void
gbk_inotify_sub_free (GbkInotifySub *sub)
{
  if (sub == nullptr)
    return;
  g_mutex_lock (&hub_lock);
  for (guint i = 0; i < sub->wds->len; i++)
    {
      int wd = g_array_index (sub->wds, int, i);
      GPtrArray *subs = g_hash_table_lookup (wd_subs, GINT_TO_POINTER (wd));
      if (subs == nullptr)
        continue;                   /* the kernel already dropped it (IN_IGNORED) */
      g_ptr_array_remove_fast (subs, sub);
      if (subs->len == 0)
        {
          g_hash_table_remove (wd_subs, GINT_TO_POINTER (wd));
          inotify_rm_watch (hub_fd, wd);
        }
    }
  g_mutex_unlock (&hub_lock);
  /* Unregistered: the hub can no longer deliver to us. */
  g_array_unref (sub->wds);
  g_array_unref (sub->events);
  close (sub->efd);
  g_mutex_clear (&sub->lock);
  g_free (sub);
}

void
gbk_inotify_sub_take (GbkInotifySub *sub, GbkInotifyFn fn, void *data)
{
  uint64_t v;
  while (read (sub->efd, &v, sizeof v) < 0 && errno == EINTR)
    ;
  g_mutex_lock (&sub->lock);
  GArray *events = sub->events;
  sub->events = g_array_new (false, false, sizeof (Event));
  g_array_set_clear_func (sub->events, event_clear);
  bool overflow = sub->overflow;
  sub->overflow = false;
  g_mutex_unlock (&sub->lock);

  if (overflow)
    fn (-1, IN_Q_OVERFLOW, nullptr, 0, data);
  for (guint i = 0; i < events->len; i++)
    {
      const Event *e = &g_array_index (events, Event, i);
      fn (e->wd, e->mask, e->name, e->name != nullptr ? strlen (e->name) : 0, data);
    }
  g_array_unref (events);
}
