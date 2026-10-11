#include "fileops.h"

#define COPY_CHUNK (4u << 20)
#define FALLBACK_BUF (1u << 20)
#define TMP_PREFIX ".gbk-tmp-"

typedef struct
{
  const GbkOpsHooks *h;
  bool cancelled;
  bool have_all;          /* "apply to all" was chosen */
  GbkChoice all_choice;
  bool incomplete;        /* something inside the current top-level item was skipped */
  uint64_t done, total;
} Ctx;

/* ---- small helpers ---- */

static bool
cancelled (Ctx *c)
{
  if (!c->cancelled && c->h->cancelled != nullptr && c->h->cancelled (c->h->data))
    c->cancelled = true;
  return c->cancelled;
}

static void
progress (Ctx *c, const char *current)
{
  if (c->h->progress != nullptr)
    c->h->progress (c->done, c->total, current, c->h->data);
}

/* Report errno for `path`; returns true to carry on (skip), false to stop. */
static bool
fail_errno (Ctx *c, const char *path, int err, const char *what)
{
  c->incomplete = true;
  g_autofree char *shown = g_filename_display_name (path);
  g_autoptr (GError) e = g_error_new (G_IO_ERROR, g_io_error_from_errno (err), "%s %s: %s",
                                      what, shown, g_strerror (err));
  GbkChoice ch = c->h->error != nullptr ? c->h->error (path, e, c->h->data) : GBK_CHOICE_CANCEL;
  if (ch == GBK_CHOICE_CANCEL)
    c->cancelled = true;
  return !c->cancelled;
}

static bool
fail_msg (Ctx *c, const char *path, GIOErrorEnum code, const char *msg)
{
  c->incomplete = true;
  g_autoptr (GError) e = g_error_new_literal (G_IO_ERROR, code, msg);
  GbkChoice ch = c->h->error != nullptr ? c->h->error (path, e, c->h->data) : GBK_CHOICE_CANCEL;
  if (ch == GBK_CHOICE_CANCEL)
    c->cancelled = true;
  return !c->cancelled;
}

static GbkChoice
ask_conflict (Ctx *c, const char *src, const char *dst, bool dst_is_dir)
{
  if (c->have_all)
    return c->all_choice;
  bool all = false;
  GbkChoice ch = c->h->conflict != nullptr ? c->h->conflict (src, dst, dst_is_dir, &all, c->h->data)
                                           : GBK_CHOICE_SKIP;
  if (ch == GBK_CHOICE_CANCEL)
    c->cancelled = true;
  else if (all)
    {
      c->have_all = true;
      c->all_choice = ch;
    }
  return ch;
}

static bool
same_inode (const struct stat *a, const struct stat *b)
{
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

bool
gbk_ops_valid_name (const char *name, GError **error)
{
  if (name[0] == '\0' || g_str_equal (name, ".") || g_str_equal (name, "..")
      || strchr (name, '/') != nullptr)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                   "\"%s\" is not a valid file name", name);
      return false;
    }
  if (strlen (name) > 255)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FILENAME_TOO_LONG, "Name too long");
      return false;
    }
  return true;
}

char *
gbk_ops_free_name (const char *dir, const char *name)
{
  /* Split stem/extension: "a.txt" -> "a (1).txt", "x.tar.gz" -> "x (1).tar.gz",
   * ".bashrc" -> ".bashrc (1)". */
  const char *ext = strstr (name + 1, ".tar.");
  if (ext == nullptr)
    {
      ext = strrchr (name + 1, '.');
      if (ext == nullptr)
        ext = name + strlen (name);
    }
  g_autofree char *stem = g_strndup (name, (gsize) (ext - name));
  for (unsigned n = 1; n < 100000; n++)
    {
      g_autofree char *candidate = g_strdup_printf ("%s (%u)%s", stem, n, ext);
      g_autofree char *path = g_build_filename (dir, candidate, nullptr);
      struct stat st;
      if (lstat (path, &st) != 0 && errno == ENOENT)
        return g_steal_pointer (&candidate);
    }
  return g_strdup_printf ("%s (%" G_GINT64_FORMAT ")%s", stem, g_get_real_time (), ext);
}

/* Total work for progress: bytes of regular files (copy/move) or entries (trash/delete). */
static void
measure (Ctx *c, const char *path, bool bytes, int depth)
{
  struct stat st;
  if (cancelled (c) || depth > GBK_OPS_MAX_DEPTH || lstat (path, &st) != 0)
    return;
  c->total += bytes ? (S_ISREG (st.st_mode) ? (uint64_t) st.st_size : 0) : 1;
  if (!S_ISDIR (st.st_mode))
    return;
  g_autoptr (GDir) d = g_dir_open (path, 0, nullptr);
  const char *name;
  while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
    {
      g_autofree char *child = g_build_filename (path, name, nullptr);
      measure (c, child, bytes, depth + 1);
    }
}

/* Is `inner` the same as, or inside, `outer`? (After resolving both.) */
static bool
is_within (const char *inner, const char *outer)
{
  g_autofree char *ri = realpath (inner, nullptr);
  g_autofree char *ro = realpath (outer, nullptr);
  if (ri == nullptr || ro == nullptr)
    return false;
  size_t n = strlen (ro);
  return strncmp (ri, ro, n) == 0 && (ri[n] == '\0' || ri[n] == '/' || g_str_equal (ro, "/"));
}

/* ---- copying ---- */

/* Copy the bytes of open `in` to open `out`, with cancellation and progress. */
static int
copy_data (Ctx *c, int in, int out, const char *shown)
{
  bool use_cfr = true;
  g_autofree char *buf = nullptr;
  for (;;)
    {
      if (cancelled (c))
        return ECANCELED;
      ssize_t n;
      if (use_cfr)
        {
          n = copy_file_range (in, nullptr, out, nullptr, COPY_CHUNK, 0);
          if (n < 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS
                        || errno == EOPNOTSUPP || errno == EBADF))
            {
              use_cfr = false;     /* not supported here: plain read/write */
              continue;
            }
        }
      else
        {
          if (buf == nullptr)
            buf = g_malloc (FALLBACK_BUF);
          n = read (in, buf, FALLBACK_BUF);
          if (n > 0)
            {
              for (ssize_t off = 0; off < n;)
                {
                  ssize_t w = write (out, buf + off, (size_t) (n - off));
                  if (w < 0)
                    {
                      if (errno == EINTR)
                        continue;
                      return errno;
                    }
                  off += w;
                }
            }
        }
      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          return errno;
        }
      if (n == 0)
        return 0;
      c->done += (uint64_t) n;
      progress (c, shown);
    }
}

/* Create `dst` as a copy of regular file `src` via a temp file in dst's directory, then an
 * atomic rename. `replace`: dst exists and the user chose to replace it. */
static bool
copy_file (Ctx *c, const char *src, const char *dst, const struct stat *st, bool replace)
{
  int in = open (src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (in < 0)
    return fail_errno (c, src, errno, "Can't read");

  g_autofree char *dir = g_path_get_dirname (dst);
  g_autofree char *tmp = g_build_filename (dir, TMP_PREFIX "XXXXXX", nullptr);
  int out = g_mkstemp_full (tmp, O_WRONLY | O_CLOEXEC, 0600);
  if (out < 0)
    {
      int err = errno;
      close (in);
      return fail_errno (c, dst, err, "Can't create");
    }

  g_autofree char *shown = g_filename_display_name (src);
  int err = copy_data (c, in, out, shown);
  if (err == 0 && fchmod (out, st->st_mode & 07777) != 0)
    err = errno;
  struct timespec times[2] = { st->st_atim, st->st_mtim };
  if (err == 0 && futimens (out, times) != 0)
    err = errno;
  /* Replacing: make the data durable before the old file disappears. */
  if (err == 0 && replace && fsync (out) != 0)
    err = errno;
  if (close (out) != 0 && err == 0)
    err = errno;
  close (in);

  if (err == 0)
    {
      int r = replace ? rename (tmp, dst)
                      : renameat2 (AT_FDCWD, tmp, AT_FDCWD, dst, RENAME_NOREPLACE);
      if (r != 0)
        err = errno;
    }
  if (err != 0)
    {
      unlink (tmp);
      if (err == ECANCELED)
        return false;
      return fail_errno (c, dst, err, "Can't write");
    }
  return true;
}

static bool
copy_symlink (Ctx *c, const char *src, const char *dst, bool replace)
{
  g_autoptr (GError) e = nullptr;
  g_autofree char *target = g_file_read_link (src, &e);
  if (target == nullptr)
    return fail_errno (c, src, EIO, "Can't read link");
  if (!replace)
    {
      if (symlink (target, dst) != 0)
        return fail_errno (c, dst, errno, "Can't create link");
      return true;
    }
  g_autofree char *dir = g_path_get_dirname (dst);
  g_autofree char *base = g_strdup_printf (TMP_PREFIX "%" G_GINT64_FORMAT, g_get_real_time ());
  g_autofree char *tmp = g_build_filename (dir, base, nullptr);
  if (symlink (target, tmp) != 0)
    return fail_errno (c, dst, errno, "Can't create link");
  if (rename (tmp, dst) != 0)
    {
      int err = errno;
      unlink (tmp);
      return fail_errno (c, dst, err, "Can't replace");
    }
  return true;
}

static bool copy_into (Ctx *c, const char *src, const char *dst_dir, const char *name,
                       int depth, GPtrArray *created);

static bool
copy_dir (Ctx *c, const char *src, const char *dst, const struct stat *st, bool merge, int depth)
{
  if (!merge && mkdir (dst, 0700) != 0)     /* owner-only while we fill it */
    return fail_errno (c, dst, errno, "Can't create folder");
  g_autoptr (GError) e = nullptr;
  g_autoptr (GDir) d = g_dir_open (src, 0, &e);
  if (d == nullptr)
    return fail_errno (c, src, EACCES, "Can't read folder");
  const char *name;
  while ((name = g_dir_read_name (d)) != nullptr)
    {
      g_autofree char *child = g_build_filename (src, name, nullptr);
      if (!copy_into (c, child, dst, name, depth + 1, nullptr))
        return false;
    }
  if (!merge)
    {
      chmod (dst, st->st_mode & 07777);
      struct timespec times[2] = { st->st_atim, st->st_mtim };
      utimensat (AT_FDCWD, dst, times, 0);
    }
  return true;
}

/* Copy `src` to dst_dir/name, resolving conflicts. Returns false to stop everything. */
static bool
copy_into (Ctx *c, const char *src, const char *dst_dir, const char *name, int depth,
           GPtrArray *created)
{
  if (cancelled (c))
    return false;
  if (depth > GBK_OPS_MAX_DEPTH)
    return fail_msg (c, src, G_IO_ERROR_FAILED, "Folder structure is too deep");

  struct stat st, dst_st;
  if (lstat (src, &st) != 0)
    return fail_errno (c, src, errno, "Can't read");

  g_autofree char *final_name = g_strdup (name);
  g_autofree char *dst = g_build_filename (dst_dir, name, nullptr);
  bool replace = false;
  if (lstat (dst, &dst_st) == 0)
    {
      GbkChoice ch = same_inode (&st, &dst_st)
                       ? GBK_CHOICE_KEEP_BOTH       /* pasted where it came from: a copy */
                       : ask_conflict (c, src, dst, S_ISDIR (dst_st.st_mode));
      switch (ch)
        {
        case GBK_CHOICE_CANCEL:
          return false;
        case GBK_CHOICE_SKIP:
          c->incomplete = true;
          return true;
        case GBK_CHOICE_KEEP_BOTH:
          g_free (final_name);
          final_name = gbk_ops_free_name (dst_dir, name);
          g_free (dst);
          dst = g_build_filename (dst_dir, final_name, nullptr);
          break;
        case GBK_CHOICE_REPLACE:
          if (S_ISDIR (st.st_mode) != S_ISDIR (dst_st.st_mode))
            return fail_msg (c, dst, G_IO_ERROR_EXISTS,
                             S_ISDIR (st.st_mode) ? "A file is in the way of this folder"
                                                  : "A folder is in the way of this file");
          replace = true;
          break;
        default:
          gbk_fatal ("bad choice %d", (int) ch);
        }
    }

  bool ok;
  if (S_ISDIR (st.st_mode))
    ok = copy_dir (c, src, dst, &st, replace, depth);
  else if (S_ISLNK (st.st_mode))
    ok = copy_symlink (c, src, dst, replace);
  else if (S_ISREG (st.st_mode))
    ok = copy_file (c, src, dst, &st, replace);
  else
    ok = fail_msg (c, src, G_IO_ERROR_NOT_SUPPORTED, "Special files can't be copied");

  if (ok && !cancelled (c) && created != nullptr)
    g_ptr_array_add (created, g_strdup (final_name));
  return ok && !cancelled (c);
}

/* ---- deleting ---- */

static bool
delete_at (Ctx *c, int parent, const char *name, const char *path, int depth)
{
  if (cancelled (c))
    return false;
  if (depth > GBK_OPS_MAX_DEPTH)
    return fail_msg (c, path, G_IO_ERROR_FAILED, "Folder structure is too deep");
  struct stat st;
  if (fstatat (parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT ? true : fail_errno (c, path, errno, "Can't delete");

  if (S_ISDIR (st.st_mode))
    {
      int fd = openat (parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      DIR *d = fd >= 0 ? fdopendir (fd) : nullptr;
      if (d == nullptr)
        {
          int err = errno;
          if (fd >= 0)
            close (fd);
          return fail_errno (c, path, err, "Can't delete");
        }
      struct dirent *de;
      bool ok = true;
      while (ok && (de = readdir (d)) != nullptr)
        {
          if (g_str_equal (de->d_name, ".") || g_str_equal (de->d_name, ".."))
            continue;
          g_autofree char *child = g_build_filename (path, de->d_name, nullptr);
          ok = delete_at (c, dirfd (d), de->d_name, child, depth + 1);
        }
      closedir (d);
      if (!ok)
        return false;
      if (unlinkat (parent, name, AT_REMOVEDIR) != 0)
        return fail_errno (c, path, errno, "Can't delete");
    }
  else if (unlinkat (parent, name, 0) != 0)
    return fail_errno (c, path, errno, "Can't delete");

  c->done++;
  progress (c, path);
  return true;
}

static bool
delete_path (Ctx *c, const char *path)
{
  g_autofree char *dir = g_path_get_dirname (path);
  g_autofree char *base = g_path_get_basename (path);
  int parent = open (dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent < 0)
    return fail_errno (c, path, errno, "Can't delete");
  bool ok = delete_at (c, parent, base, path, 0);
  close (parent);
  return ok;
}

/* ---- moving ---- */

static bool move_into (Ctx *c, const char *src, const char *dst_dir, const char *name,
                       int depth, GPtrArray *created);

/* Different filesystem: copy, and delete the source only if the copy was complete. */
static bool
move_by_copy (Ctx *c, const char *src, const char *dst_dir, const char *name, int depth)
{
  bool was_incomplete = c->incomplete;
  c->incomplete = false;
  bool ok = copy_into (c, src, dst_dir, name, depth, nullptr);
  bool complete = ok && !c->incomplete;
  c->incomplete = c->incomplete || was_incomplete;
  if (complete)
    return delete_path (c, src);
  return ok;
}

static bool
move_into (Ctx *c, const char *src, const char *dst_dir, const char *name, int depth,
           GPtrArray *created)
{
  if (cancelled (c))
    return false;
  if (depth > GBK_OPS_MAX_DEPTH)
    return fail_msg (c, src, G_IO_ERROR_FAILED, "Folder structure is too deep");
  struct stat st, dst_st;
  if (lstat (src, &st) != 0)
    return fail_errno (c, src, errno, "Can't move");

  g_autofree char *final_name = g_strdup (name);
  g_autofree char *dst = g_build_filename (dst_dir, name, nullptr);
  if (lstat (dst, &dst_st) == 0)
    {
      if (same_inode (&st, &dst_st))
        return true;                          /* already there */
      GbkChoice ch = ask_conflict (c, src, dst, S_ISDIR (dst_st.st_mode));
      switch (ch)
        {
        case GBK_CHOICE_CANCEL:
          return false;
        case GBK_CHOICE_SKIP:
          c->incomplete = true;
          return true;
        case GBK_CHOICE_KEEP_BOTH:
          g_free (final_name);
          final_name = gbk_ops_free_name (dst_dir, name);
          g_free (dst);
          dst = g_build_filename (dst_dir, final_name, nullptr);
          break;
        case GBK_CHOICE_REPLACE:
          if (S_ISDIR (st.st_mode) != S_ISDIR (dst_st.st_mode))
            return fail_msg (c, dst, G_IO_ERROR_EXISTS,
                             S_ISDIR (st.st_mode) ? "A file is in the way of this folder"
                                                  : "A folder is in the way of this file");
          if (S_ISDIR (st.st_mode))
            {
              /* Merge: move each child in, then remove the source if it ended up empty. */
              g_autoptr (GDir) d = g_dir_open (src, 0, nullptr);
              if (d == nullptr)
                return fail_errno (c, src, EACCES, "Can't read folder");
              const char *child;
              while ((child = g_dir_read_name (d)) != nullptr)
                {
                  g_autofree char *cs = g_build_filename (src, child, nullptr);
                  if (!move_into (c, cs, dst, child, depth + 1, nullptr))
                    return false;
                }
              rmdir (src);                    /* fails harmlessly if something was skipped */
              if (created != nullptr)
                g_ptr_array_add (created, g_strdup (final_name));
              return true;
            }
          /* File over file: rename(2) replaces atomically. */
          if (rename (src, dst) == 0)
            {
              c->done += (uint64_t) st.st_size;
              if (created != nullptr)
                g_ptr_array_add (created, g_strdup (final_name));
              return true;
            }
          if (errno != EXDEV)
            return fail_errno (c, src, errno, "Can't move");
          if (!copy_file (c, src, dst, &st, true))
            return false;
          if (created != nullptr)
            g_ptr_array_add (created, g_strdup (final_name));
          return delete_path (c, src);
        default:
          gbk_fatal ("bad choice %d", (int) ch);
        }
    }

  if (renameat2 (AT_FDCWD, src, AT_FDCWD, dst, RENAME_NOREPLACE) == 0)
    {
      c->done += S_ISREG (st.st_mode) ? (uint64_t) st.st_size : 0;
      progress (c, src);
    }
  else if (errno == EXDEV)
    {
      if (!move_by_copy (c, src, dst_dir, final_name, depth))
        return false;
    }
  else
    return fail_errno (c, src, errno, "Can't move");

  if (created != nullptr)
    g_ptr_array_add (created, g_strdup (final_name));
  return !cancelled (c);
}

/* ---- trash ---- */

static bool
trash_path (Ctx *c, const char *path)
{
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_autoptr (GError) e = nullptr;
  if (g_file_trash (f, nullptr, &e))
    {
      c->done++;
      progress (c, path);
      return true;
    }
  if (g_error_matches (e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    {
      if (c->h->delete_untrashable != nullptr && c->h->delete_untrashable (path, c->h->data))
        return delete_path (c, path);
      c->incomplete = true;
      return !cancelled (c);
    }
  c->incomplete = true;
  GbkChoice ch = c->h->error != nullptr ? c->h->error (path, e, c->h->data) : GBK_CHOICE_CANCEL;
  if (ch == GBK_CHOICE_CANCEL)
    c->cancelled = true;
  return !c->cancelled;
}

/* ---- entry points ---- */

bool
gbk_ops_run (GbkOpKind kind, GPtrArray *sources, const char *dest_dir,
             const GbkOpsHooks *hooks, GPtrArray *created, GError **error)
{
  Ctx c = { .h = hooks };
  bool bytes = kind == GBK_OP_COPY || kind == GBK_OP_MOVE;

  if (bytes)
    {
      struct stat st;
      if (stat (dest_dir, &st) != 0 || !S_ISDIR (st.st_mode))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                       "Destination is not a folder");
          return false;
        }
      for (guint i = 0; i < sources->len; i++)
        {
          const char *src = g_ptr_array_index (sources, i);
          struct stat sst;
          if (lstat (src, &sst) == 0 && S_ISDIR (sst.st_mode) && is_within (dest_dir, src))
            {
              g_autofree char *shown = g_filename_display_name (src);
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Can't put \"%s\" inside itself", shown);
              return false;
            }
        }
    }

  for (guint i = 0; i < sources->len; i++)
    measure (&c, g_ptr_array_index (sources, i), bytes, 0);
  progress (&c, "");

  for (guint i = 0; i < sources->len && !cancelled (&c); i++)
    {
      const char *src = g_ptr_array_index (sources, i);
      g_autofree char *name = g_path_get_basename (src);
      c.incomplete = false;
      switch (kind)
        {
        case GBK_OP_COPY: copy_into (&c, src, dest_dir, name, 0, created); break;
        case GBK_OP_MOVE: move_into (&c, src, dest_dir, name, 0, created); break;
        case GBK_OP_TRASH: trash_path (&c, src); break;
        case GBK_OP_DELETE: delete_path (&c, src); break;
        default: gbk_fatal ("bad op %d", (int) kind);
        }
    }

  if (c.cancelled)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      return false;
    }
  return true;
}

bool
gbk_ops_rename (const char *path, const char *new_name, GError **error)
{
  if (!gbk_ops_valid_name (new_name, error))
    return false;
  g_autofree char *dir = g_path_get_dirname (path);
  g_autofree char *dst = g_build_filename (dir, new_name, nullptr);
  if (g_str_equal (dst, path))
    return true;

  struct stat a, b;
  bool same_file = lstat (path, &a) == 0 && lstat (dst, &b) == 0 && same_inode (&a, &b);
  /* Same file under another spelling (case change on a case-insensitive filesystem): a
   * plain rename is safe. Otherwise never replace. */
  int r = same_file ? rename (path, dst)
                    : renameat2 (AT_FDCWD, path, AT_FDCWD, dst, RENAME_NOREPLACE);
  if (r != 0)
    {
      int err = errno;
      g_autofree char *shown = g_filename_display_name (new_name);
      if (err == EEXIST)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS, "\"%s\" already exists", shown);
      else
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err), "Can't rename to \"%s\": %s",
                     shown, g_strerror (err));
      return false;
    }
  return true;
}
