/* File operations: copy, move, trash, delete, rename. Blocking: run on a worker thread
 * (see job.h), never on the main thread.
 *
 * Safety rules:
 * - Never overwrite without asking. New names are created with O_EXCL / RENAME_NOREPLACE,
 *   so something appearing mid-operation becomes a conflict, not a silent overwrite.
 * - Replacing a file is atomic: data goes to a hidden temp file in the destination
 *   directory, is fsync'd, then renamed over the old one. A failed or cancelled copy never
 *   leaves a half-written file where a good one was.
 * - Symlinks are copied/moved/deleted as links, never followed. Deletion walks the tree
 *   with dirfds (openat/unlinkat, O_NOFOLLOW): a symlink can't redirect it elsewhere.
 * - A folder can't be copied or moved into itself.
 * - Directory depth is capped (GBK_OPS_MAX_DEPTH): deep trees fail cleanly instead of
 *   overflowing the stack. */
#pragma once

#define GBK_OPS_MAX_DEPTH 512

typedef enum
{
  GBK_OP_COPY,
  GBK_OP_MOVE,
  GBK_OP_TRASH,
  GBK_OP_DELETE,       /* permanent */
} GbkOpKind;

typedef enum
{
  GBK_CHOICE_REPLACE,   /* files: overwrite; folders: merge */
  GBK_CHOICE_SKIP,
  GBK_CHOICE_KEEP_BOTH, /* the new one gets a free "name (1).ext" */
  GBK_CHOICE_CANCEL,
} GbkChoice;

typedef struct
{
  /* `dst` exists. Return the choice; set *apply_all to reuse it for later conflicts. */
  GbkChoice (*conflict) (const char *src, const char *dst, bool dst_is_dir, bool *apply_all,
                         void *data);
  /* Something failed for `path`: GBK_CHOICE_SKIP to continue, GBK_CHOICE_CANCEL to stop. */
  GbkChoice (*error) (const char *path, const GError *error, void *data);
  /* Trash isn't possible for `path` (e.g. no trash on that filesystem): delete instead? */
  bool (*delete_untrashable) (const char *path, void *data);
  void (*progress) (uint64_t bytes_done, uint64_t bytes_total, const char *current, void *data);
  bool (*cancelled) (void *data);
  void *data;
} GbkOpsHooks;

/* Run `kind` on `sources` (absolute paths). `dest_dir` is used by COPY and MOVE.
 * Returns false only on cancellation or a fatal precondition (error set); per-item failures
 * go through hooks->error. `created` (may be nullptr) receives the basenames created in
 * dest_dir (char*, owned) so the caller can select them. */
bool gbk_ops_run (GbkOpKind kind, GPtrArray *sources, const char *dest_dir,
                  const GbkOpsHooks *hooks, GPtrArray *created, GError **error);

/* Rename `path` to `new_name` in the same directory. Fails (G_IO_ERROR_EXISTS) rather than
 * replace an existing file. */
bool gbk_ops_rename (const char *path, const char *new_name, GError **error);

/* Is `name` acceptable as a single filename? (non-empty, no '/', not "." or "..") */
bool gbk_ops_valid_name (const char *name, GError **error);

/* "name (1).ext" style alternative for `name` that doesn't exist in `dir`. */
[[nodiscard]] char *gbk_ops_free_name (const char *dir, const char *name);
