/* GCC -fanalyzer build only (GBK_ANALYZER, from meson's analyzer option): tell the analyzer
 * which deallocator frees what GLib's allocators return, so it checks g_free() as it checks
 * free() — double free, use after free, leaks — and still understands g_autofree (it runs
 * cleanup functions; the Clang analyzer does not). GLib does not declare these itself.
 *
 * Analyzer build only: returns_nonnull is a promise the optimiser may act on, and
 * g_malloc (0) does return NULL. The analyzer reads it as "never NULL", which is true of
 * every allocation here with a size above zero; without it, every dereference of
 * g_malloc's result reads as a possible NULL.
 *
 * Leaks are not reported (-Wno-analyzer-malloc-leak in meson.build): GCC 15.2 loses an
 * allocation freed through a cleanup function taking `void *`, as g_autofree's does, when
 * the value came from a function in the same file, and reports it leaked. LeakSanitizer
 * checks leaks on every path the tests run.
 *
 * Parenthesised names: several are macros (g_strdup -> g_strdup_inline, g_free -> a choice
 * between g_free_sized and g_free). A function missing here is simply not checked. */
#pragma once

/* GLib's inline g_strdup wrapper: a g_autofree variable assigned from it reads as leaked
 * (GCC 15.2; the same with the real function is fine). */
#undef g_strdup

#define GBK_FREED_BY_G_FREE malloc (g_free, 1), malloc (g_free_sized, 1)
/* Never NULL (for a size above zero; g_strdup and friends for a non-NULL argument: the
 * analyzer can't express "NULL exactly when the argument is", and treating every copy of a
 * string as possibly NULL buries the real findings). */
#define GBK_GALLOC __attribute__ ((malloc, GBK_FREED_BY_G_FREE, returns_nonnull))
/* NULL on failure. */
#define GBK_GALLOC_MAYBE __attribute__ ((malloc, GBK_FREED_BY_G_FREE))

extern gpointer (g_malloc) (gsize n_bytes) GBK_GALLOC;
extern gpointer (g_malloc0) (gsize n_bytes) GBK_GALLOC;
extern gpointer (g_malloc_n) (gsize n_blocks, gsize n_block_bytes) GBK_GALLOC;
extern gpointer (g_malloc0_n) (gsize n_blocks, gsize n_block_bytes) GBK_GALLOC;
extern gchar *(g_strdup_printf) (const gchar *format, ...) GBK_GALLOC;
extern gchar *(g_strdup_vprintf) (const gchar *format, va_list args) GBK_GALLOC;
extern gchar *(g_build_filename) (const gchar *first, ...) GBK_GALLOC;
extern gchar *(g_path_get_dirname) (const gchar *file_name) GBK_GALLOC;
extern gchar *(g_path_get_basename) (const gchar *file_name) GBK_GALLOC;
extern gchar *(g_filename_display_name) (const gchar *filename) GBK_GALLOC;
extern gchar *(g_filename_display_basename) (const gchar *filename) GBK_GALLOC;
extern gchar *(g_canonicalize_filename) (const gchar *filename, const gchar *relative_to) GBK_GALLOC;
extern gchar *(g_ascii_strdown) (const gchar *str, gssize len) GBK_GALLOC;
extern gchar *(g_utf8_casefold) (const gchar *str, gssize len) GBK_GALLOC;
extern gchar *(g_utf8_make_valid) (const gchar *str, gssize len) GBK_GALLOC;
extern gchar *(g_format_size_full) (guint64 size, GFormatSizeFlags flags) GBK_GALLOC;
extern gchar *(g_strjoinv) (const gchar *separator, gchar **str_array) GBK_GALLOC;
extern gchar *(g_get_current_dir) (void) GBK_GALLOC;
extern gchar *(g_strconcat) (const gchar *string1, ...) GBK_GALLOC;

extern gchar *(g_strdup) (const gchar *str) GBK_GALLOC;
extern gchar *(g_strndup) (const gchar *str, gsize n) GBK_GALLOC;
extern gpointer (g_memdup2) (gconstpointer mem, gsize byte_size) GBK_GALLOC;

extern gchar *(g_utf8_normalize) (const gchar *str, gssize len, GNormalizeMode mode) GBK_GALLOC_MAYBE;
extern gchar *(g_filename_to_uri) (const gchar *filename, const gchar *hostname, GError **error) GBK_GALLOC_MAYBE;
extern gchar *(g_filename_from_uri) (const gchar *uri, gchar **hostname, GError **error) GBK_GALLOC_MAYBE;

extern gchar **(g_strsplit) (const gchar *string, const gchar *delimiter, gint max_tokens)
  __attribute__ ((malloc, malloc (g_strfreev, 1), returns_nonnull));
