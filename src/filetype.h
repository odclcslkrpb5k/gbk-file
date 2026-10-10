/* File type table: filename extension -> MIME type. No content sniffing, ever.
 *
 * Rules (docs/claude-notes.md):
 * - Only "*.ext" globs are used; literal-name globs (Makefile, core, ...) are ignored.
 * - A leading dot does not start an extension: ".bashrc" has none.
 * - Longest extension wins: "a.tar.gz" matches "tar.gz" before "gz".
 * - Matching is ASCII case-insensitive, except globs flagged case-sensitive ("cs" in
 *   globs2, e.g. *.C vs *.c), which match exactly and win ties at the same length.
 * - Among globs for the same extension, the highest weight wins; ties go to the first added.
 * - A regular file with no matching extension is "executable" if it has an exec bit,
 *   otherwise "unknown".
 *
 * A finished GbkTypeTable is immutable and atomically refcounted: share it across threads. */
#pragma once

typedef uint16_t GbkTypeId;

/* Built-in types, always present at these ids. */
enum : GbkTypeId
{
  GBK_TYPE_UNKNOWN = 0,         /* application/octet-stream */
  GBK_TYPE_DIRECTORY,           /* inode/directory */
  GBK_TYPE_EXECUTABLE,          /* application/x-executable */
  GBK_TYPE_BROKEN_LINK,         /* inode/symlink */
  GBK_TYPE_CHARDEV,             /* inode/chardevice */
  GBK_TYPE_BLOCKDEV,            /* inode/blockdevice */
  GBK_TYPE_FIFO,                /* inode/fifo */
  GBK_TYPE_SOCKET,              /* inode/socket */
  GBK_TYPE_N_BUILTIN
};

/* What a directory entry is, after following symlinks. */
typedef enum : uint8_t
{
  GBK_KIND_REGULAR,
  GBK_KIND_DIRECTORY,
  GBK_KIND_CHARDEV,
  GBK_KIND_BLOCKDEV,
  GBK_KIND_FIFO,
  GBK_KIND_SOCKET,
  GBK_KIND_BROKEN_LINK,
} GbkFileKind;

/* Longest extension (bytes) the table will accept. Longer ones can never match. */
#define GBK_TYPE_MAX_EXT 64

typedef struct GbkTypeBuilder GbkTypeBuilder;
typedef struct GbkTypeTable GbkTypeTable;

GbkTypeBuilder *gbk_type_builder_new (void);
void gbk_type_builder_free (GbkTypeBuilder *b);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkTypeBuilder, gbk_type_builder_free)

/* Add one mapping. Returns false (and adds nothing) if `ext` or `mime` is malformed.
 * `ext` has no leading dot ("tar.gz"). Weight is clamped to 0..1000. */
bool gbk_type_builder_add (GbkTypeBuilder *b, const char *ext, const char *mime,
                           int weight, bool case_sensitive);

/* Parse shared-mime-info globs2 data (need not be NUL-terminated). Lines that are malformed
 * or aren't plain "*.ext" globs are skipped. Returns the number of mappings added. */
size_t gbk_type_builder_load_globs2 (GbkTypeBuilder *b, const char *data, size_t len);

/* Parse shared-mime-info generic-icons data ("mime:icon-name" lines). */
void gbk_type_builder_load_generic_icons (GbkTypeBuilder *b, const char *data, size_t len);

/* Consumes the builder. */
[[nodiscard]] GbkTypeTable *gbk_type_builder_finish (GbkTypeBuilder *b);

/* Table from the user's and system's shared-mime-info databases (XDG data dirs). */
[[nodiscard]] GbkTypeTable *gbk_type_table_new_system (void);

GbkTypeTable *gbk_type_table_ref (GbkTypeTable *t);
void gbk_type_table_unref (GbkTypeTable *t);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkTypeTable, gbk_type_table_unref)

/* `name` is a raw filename (bytes, not necessarily UTF-8) of `name_len` bytes. */
GbkTypeId gbk_type_table_lookup (const GbkTypeTable *t, const char *name, size_t name_len,
                                 GbkFileKind kind, bool executable);

size_t gbk_type_table_size (const GbkTypeTable *t);
/* Ids outside the table are a fatal programming error. */
const char *gbk_type_table_mime (const GbkTypeTable *t, GbkTypeId id);
/* Theme icon names: specific ("application-zip") and generic ("package-x-generic"). */
const char *gbk_type_table_icon (const GbkTypeTable *t, GbkTypeId id);
const char *gbk_type_table_generic_icon (const GbkTypeTable *t, GbkTypeId id);
