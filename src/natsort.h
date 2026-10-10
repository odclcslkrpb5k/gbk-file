/* Natural, case-insensitive filename ordering ("file2" < "File10").
 *
 * Sorting compares precomputed keys, never raw names: the key is computed once per file
 * (on the scanner thread), the comparison is a cheap byte walk. */
#pragma once

/* Sort key for a display name (valid UTF-8, `len` bytes): NFD-normalised and case-folded.
 * ASCII-only names take a fast path. Free with g_free. */
[[nodiscard]] char *gbk_sort_key_new (const char *display, size_t len, size_t *key_len);

/* Compare two keys: <0, 0, >0. Runs of ASCII digits compare by numeric value (any length);
 * equal values with fewer leading zeros sort first, as a final tiebreak. Everything else
 * compares bytewise, which on UTF-8 is code point order. This is a total preorder
 * (transitive, antisymmetric up to equivalence); callers break remaining ties on the raw
 * name. */
int gbk_natural_compare (const char *a, size_t alen, const char *b, size_t blen);
