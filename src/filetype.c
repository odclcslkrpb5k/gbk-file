#include "filetype.h"

#define MAX_WEIGHT 1000
#define MAX_MIME_LEN 255

static const struct
{
  const char *mime;
  const char *generic_icon;
} builtins[GBK_TYPE_N_BUILTIN] = {
  [GBK_TYPE_UNKNOWN]     = { "application/octet-stream", "text-x-generic" },
  [GBK_TYPE_DIRECTORY]   = { "inode/directory",          "folder" },
  [GBK_TYPE_EXECUTABLE]  = { "application/x-executable", "application-x-executable" },
  [GBK_TYPE_BROKEN_LINK] = { "inode/symlink",            "text-x-generic" },
  [GBK_TYPE_CHARDEV]     = { "inode/chardevice",         "text-x-generic" },
  [GBK_TYPE_BLOCKDEV]    = { "inode/blockdevice",        "text-x-generic" },
  [GBK_TYPE_FIFO]        = { "inode/fifo",               "text-x-generic" },
  [GBK_TYPE_SOCKET]      = { "inode/socket",             "text-x-generic" },
};

/* Also used, unfinished, as the builder: GbkTypeBuilder is a thin wrapper so the type system
 * stops anyone from looking things up in a half-built table. */
struct GbkTypeTable
{
  GPtrArray *mimes;          /* GbkTypeId -> owned mime string */
  GHashTable *mime_ids;      /* mime (borrowed from mimes) -> id + 1 */
  GHashTable *ext_ci;        /* ASCII-lowercased ext (owned) -> packed mapping */
  GHashTable *ext_cs;        /* exact ext (owned) -> packed mapping */
  GHashTable *generic_src;   /* mime (owned) -> generic icon (owned); builder only */
  size_t max_ext;            /* longest key in either ext table */

  /* Filled by finish; indexed by id. */
  char **icons;
  char **generic_icons;
};

struct GbkTypeBuilder
{
  GbkTypeTable *t;
};

/* A mapping is packed into the hash value: weight in the high bits, id in the low 16.
 * weight + 1 keeps the value non-zero, since 0 (nullptr) means "absent". */
static inline void *
pack (GbkTypeId id, int weight)
{
  return (void *) (((uintptr_t) (weight + 1) << 16) | id);
}

static inline GbkTypeId
packed_id (const void *v)
{
  return (GbkTypeId) ((uintptr_t) v & 0xffffu);
}

static inline int
packed_weight (const void *v)
{
  return (int) ((uintptr_t) v >> 16) - 1;
}

static void
table_free_contents (GbkTypeTable *t)
{
  if (t->icons != nullptr)
    for (guint i = 0; i < t->mimes->len; i++)
      {
        g_free (t->icons[i]);
        g_free (t->generic_icons[i]);
      }
  g_free (t->icons);
  g_free (t->generic_icons);
  g_clear_pointer (&t->generic_src, g_hash_table_unref);
  g_hash_table_unref (t->ext_cs);
  g_hash_table_unref (t->ext_ci);
  g_hash_table_unref (t->mime_ids);
  g_ptr_array_unref (t->mimes);
}

static GbkTypeId
intern_mime (GbkTypeTable *t, const char *mime, bool *ok)
{
  const void *v = g_hash_table_lookup (t->mime_ids, mime);
  if (v != nullptr)
    {
      *ok = true;
      return (GbkTypeId) (GPOINTER_TO_UINT (v) - 1);
    }
  if (t->mimes->len >= UINT16_MAX)
    {
      *ok = false;
      return GBK_TYPE_UNKNOWN;
    }
  GbkTypeId id = (GbkTypeId) t->mimes->len;
  char *owned = g_strdup (mime);
  g_ptr_array_add (t->mimes, owned);
  g_hash_table_insert (t->mime_ids, owned, GUINT_TO_POINTER ((guint) id + 1));
  *ok = true;
  return id;
}

GbkTypeBuilder *
gbk_type_builder_new (void)
{
  GbkTypeTable *t = g_atomic_rc_box_new0 (GbkTypeTable);
  t->mimes = g_ptr_array_new_with_free_func (g_free);
  t->mime_ids = g_hash_table_new (g_str_hash, g_str_equal);
  t->ext_ci = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, nullptr);
  t->ext_cs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, nullptr);
  t->generic_src = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  for (GbkTypeId i = 0; i < GBK_TYPE_N_BUILTIN; i++)
    {
      bool ok;
      GbkTypeId id = intern_mime (t, builtins[i].mime, &ok);
      g_assert (ok && id == i);
    }

  GbkTypeBuilder *b = g_new0 (GbkTypeBuilder, 1);
  b->t = t;
  return b;
}

void
gbk_type_builder_free (GbkTypeBuilder *b)
{
  if (b == nullptr)
    return;
  if (b->t != nullptr)
    g_atomic_rc_box_release_full (b->t, (GDestroyNotify) table_free_contents);
  g_free (b);
}

static bool
valid_ext (const char *ext, size_t len)
{
  if (len == 0 || len > GBK_TYPE_MAX_EXT)
    return false;
  for (size_t i = 0; i < len; i++)
    {
      unsigned char c = (unsigned char) ext[i];
      /* No path separators, glob metacharacters, whitespace or control bytes. */
      if (c == '/' || c == '*' || c == '?' || c == '[' || c == '\\' || c <= ' ' || c == 0x7f)
        return false;
    }
  return true;
}

/* RFC 6838 restricted-name characters, "type/subtype". */
static bool
valid_mime (const char *mime)
{
  size_t len = strlen (mime);
  if (len < 3 || len > MAX_MIME_LEN)
    return false;
  const char *slash = strchr (mime, '/');
  if (slash == nullptr || slash == mime || slash[1] == '\0' || strchr (slash + 1, '/') != nullptr)
    return false;
  for (size_t i = 0; i < len; i++)
    {
      char c = mime[i];
      if (!(g_ascii_isalnum (c) || c == '/' || strchr ("!#$&^_.+-", c) != nullptr))
        return false;
    }
  return true;
}

bool
gbk_type_builder_add (GbkTypeBuilder *b, const char *ext, const char *mime,
                      int weight, bool case_sensitive)
{
  g_assert (b != nullptr && b->t != nullptr);
  GbkTypeTable *t = b->t;

  size_t ext_len = strlen (ext);
  if (!valid_ext (ext, ext_len) || !valid_mime (mime))
    return false;
  weight = CLAMP (weight, 0, MAX_WEIGHT);

  bool ok;
  GbkTypeId id = intern_mime (t, mime, &ok);
  if (!ok)
    return false;

  GHashTable *table = case_sensitive ? t->ext_cs : t->ext_ci;
  char *key = case_sensitive ? g_strdup (ext) : g_ascii_strdown (ext, (gssize) ext_len);
  const void *existing = g_hash_table_lookup (table, key);
  if (existing != nullptr && packed_weight (existing) >= weight)
    {
      g_free (key);            /* earlier mapping wins ties */
      return true;
    }
  g_hash_table_replace (table, key, pack (id, weight));
  t->max_ext = MAX (t->max_ext, ext_len);
  return true;
}

/* Calls fn(line) for each line of data, NUL-terminated and without the newline. */
static void
for_each_line (const char *data, size_t len, void (*fn) (char *line, void *user), void *user)
{
  const char *p = data;
  const char *end = data + len;
  while (p < end)
    {
      const char *nl = memchr (p, '\n', (size_t) (end - p));
      const char *line_end = nl != nullptr ? nl : end;
      g_autofree char *line = g_strndup (p, (gsize) (line_end - p));
      fn (line, user);
      p = nl != nullptr ? nl + 1 : end;
    }
}

typedef struct
{
  GbkTypeBuilder *b;
  size_t added;
} Globs2State;

static void
globs2_line (char *line, void *user)
{
  Globs2State *st = user;
  if (line[0] == '#' || line[0] == '\0')
    return;

  /* weight:mime:glob[:flags] */
  g_auto (GStrv) f = g_strsplit (line, ":", 5);
  guint n = g_strv_length (f);
  if (n < 3 || n > 4)
    return;

  guint64 weight;
  if (!g_ascii_string_to_unsigned (f[0], 10, 0, MAX_WEIGHT, &weight, nullptr))
    return;
  if (!g_str_has_prefix (f[2], "*."))
    return;                    /* literal names and complex globs: not extensions */

  bool cs = false;
  if (n == 4)
    {
      g_auto (GStrv) flags = g_strsplit (f[3], ",", 0);
      for (guint i = 0; flags[i] != nullptr; i++)
        if (g_str_equal (flags[i], "cs"))
          cs = true;
    }

  if (gbk_type_builder_add (st->b, f[2] + 2, f[1], (int) weight, cs))
    st->added++;
}

size_t
gbk_type_builder_load_globs2 (GbkTypeBuilder *b, const char *data, size_t len)
{
  Globs2State st = { .b = b };
  for_each_line (data, len, globs2_line, &st);
  return st.added;
}

static void
generic_icons_line (char *line, void *user)
{
  GbkTypeBuilder *b = user;
  char *colon = strchr (line, ':');
  if (colon == nullptr)
    return;
  *colon = '\0';
  const char *mime = line;
  const char *icon = colon + 1;
  if (!valid_mime (mime) || icon[0] == '\0' || strlen (icon) > MAX_MIME_LEN
      || strchr (icon, '/') != nullptr)
    return;
  g_hash_table_replace (b->t->generic_src, g_strdup (mime), g_strdup (icon));
}

void
gbk_type_builder_load_generic_icons (GbkTypeBuilder *b, const char *data, size_t len)
{
  g_assert (b != nullptr && b->t != nullptr);
  for_each_line (data, len, generic_icons_line, b);
}

/* freedesktop fallback by media type, as GIO does it. */
static char *
derived_generic_icon (const char *mime)
{
  static const char *const media[] = { "text", "image", "audio", "video", "font" };
  for (size_t i = 0; i < G_N_ELEMENTS (media); i++)
    {
      size_t n = strlen (media[i]);
      if (strncmp (mime, media[i], n) == 0 && mime[n] == '/')
        return g_strconcat (media[i], "-x-generic", nullptr);
    }
  return g_strdup ("application-x-generic");
}

GbkTypeTable *
gbk_type_builder_finish (GbkTypeBuilder *b)
{
  g_assert (b != nullptr && b->t != nullptr);
  GbkTypeTable *t = g_steal_pointer (&b->t);
  gbk_type_builder_free (b);

  guint n = t->mimes->len;
  t->icons = g_new0 (char *, n);
  t->generic_icons = g_new0 (char *, n);
  for (guint i = 0; i < n; i++)
    {
      const char *mime = g_ptr_array_index (t->mimes, i);
      t->icons[i] = g_strdelimit (g_strdup (mime), "/", '-');
      const char *src = g_hash_table_lookup (t->generic_src, mime);
      if (i < GBK_TYPE_N_BUILTIN)
        t->generic_icons[i] = g_strdup (builtins[i].generic_icon);
      else if (src != nullptr)
        t->generic_icons[i] = g_strdup (src);
      else
        t->generic_icons[i] = derived_generic_icon (mime);
    }
  g_clear_pointer (&t->generic_src, g_hash_table_unref);
  return t;
}

static void
load_file (GbkTypeBuilder *b, const char *dir, const char *name, bool globs)
{
  g_autofree char *path = g_build_filename (dir, "mime", name, nullptr);
  g_autofree char *data = nullptr;
  gsize len = 0;
  if (!g_file_get_contents (path, &data, &len, nullptr))
    return;
  if (globs)
    gbk_type_builder_load_globs2 (b, data, len);
  else
    gbk_type_builder_load_generic_icons (b, data, len);
}

GbkTypeTable *
gbk_type_table_new_system (void)
{
  return gbk_type_builder_finish (gbk_type_builder_new_system ());
}

GbkTypeBuilder *
gbk_type_builder_new_system (void)
{
  GbkTypeBuilder *b = gbk_type_builder_new ();
  /* User dir first: earlier mappings win weight ties. For generic icons, later wins, so
   * load in reverse. */
  const char *const *sys = g_get_system_data_dirs ();
  load_file (b, g_get_user_data_dir (), "globs2", true);
  for (size_t i = 0; sys[i] != nullptr; i++)
    load_file (b, sys[i], "globs2", true);
  size_t n_sys = 0;
  while (sys[n_sys] != nullptr)
    n_sys++;
  for (size_t i = n_sys; i > 0; i--)
    load_file (b, sys[i - 1], "generic-icons", false);
  load_file (b, g_get_user_data_dir (), "generic-icons", false);
  return b;
}

GbkTypeTable *
gbk_type_table_ref (GbkTypeTable *t)
{
  return g_atomic_rc_box_acquire (t);
}

void
gbk_type_table_unref (GbkTypeTable *t)
{
  g_atomic_rc_box_release_full (t, (GDestroyNotify) table_free_contents);
}

/* Returns the mapped id, or -1 if the extension isn't in the table. (An extension may map
 * to GBK_TYPE_UNKNOWN deliberately, e.g. via config, so that can't mean "absent".) */
static int
lookup_ext (const GbkTypeTable *t, const char *ext, size_t len)
{
  char buf[GBK_TYPE_MAX_EXT + 1];
  memcpy (buf, ext, len);
  buf[len] = '\0';
  const void *cs = g_hash_table_lookup (t->ext_cs, buf);
  for (size_t i = 0; i < len; i++)
    buf[i] = g_ascii_tolower (buf[i]);
  const void *ci = g_hash_table_lookup (t->ext_ci, buf);

  if (cs != nullptr && (ci == nullptr || packed_weight (cs) >= packed_weight (ci)))
    return packed_id (cs);
  if (ci != nullptr)
    return packed_id (ci);
  return -1;
}

GbkTypeId
gbk_type_table_lookup (const GbkTypeTable *t, const char *name, size_t name_len,
                       GbkFileKind kind, bool executable)
{
  g_assert (t != nullptr && t->icons != nullptr);

  switch (kind)
    {
    case GBK_KIND_REGULAR: break;
    case GBK_KIND_DIRECTORY: return GBK_TYPE_DIRECTORY;
    case GBK_KIND_CHARDEV: return GBK_TYPE_CHARDEV;
    case GBK_KIND_BLOCKDEV: return GBK_TYPE_BLOCKDEV;
    case GBK_KIND_FIFO: return GBK_TYPE_FIFO;
    case GBK_KIND_SOCKET: return GBK_TYPE_SOCKET;
    case GBK_KIND_BROKEN_LINK: return GBK_TYPE_BROKEN_LINK;
    default: gbk_fatal ("bad file kind %d", (int) kind);
    }

  /* Dots from left to right = extensions from longest to shortest. Position 0 is skipped:
   * a leading dot marks a hidden file, not an extension. */
  for (size_t i = 1; i < name_len; i++)
    {
      if (name[i] != '.')
        continue;
      size_t ext_len = name_len - i - 1;
      if (ext_len == 0 || ext_len > t->max_ext)
        continue;
      int id = lookup_ext (t, name + i + 1, ext_len);
      if (id >= 0)
        return (GbkTypeId) id;
    }
  return executable ? GBK_TYPE_EXECUTABLE : GBK_TYPE_UNKNOWN;
}

size_t
gbk_type_table_size (const GbkTypeTable *t)
{
  return t->mimes->len;
}

static void
check_id (const GbkTypeTable *t, GbkTypeId id)
{
  if (id >= t->mimes->len)
    gbk_fatal ("type id %u out of range (%u types)", (unsigned) id, t->mimes->len);
}

const char *
gbk_type_table_mime (const GbkTypeTable *t, GbkTypeId id)
{
  check_id (t, id);
  return g_ptr_array_index (t->mimes, id);
}

const char *
gbk_type_table_icon (const GbkTypeTable *t, GbkTypeId id)
{
  check_id (t, id);
  return t->icons[id];
}

const char *
gbk_type_table_generic_icon (const GbkTypeTable *t, GbkTypeId id)
{
  check_id (t, id);
  return t->generic_icons[id];
}
