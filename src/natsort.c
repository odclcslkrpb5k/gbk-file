#include "natsort.h"

char *
gbk_sort_key_new (const char *display, size_t len, size_t *key_len)
{
  g_assert (g_utf8_validate_len (display, len, nullptr));

  bool ascii = true;
  for (size_t i = 0; i < len && ascii; i++)
    ascii = (unsigned char) display[i] < 0x80;

  char *key;
  if (ascii)
    key = g_ascii_strdown (display, (gssize) len);
  else
    {
      /* NFD first so composed and decomposed spellings of the same name sort together. */
      g_autofree char *normal = g_utf8_normalize (display, (gssize) len, G_NORMALIZE_DEFAULT);
      key = g_utf8_casefold (normal, -1);
    }
  *key_len = strlen (key);
  return key;
}

static inline bool
is_digit (char c)
{
  return c >= '0' && c <= '9';
}

int
gbk_natural_compare (const char *a, size_t alen, const char *b, size_t blen)
{
  size_t i = 0, j = 0;
  int zeros_tiebreak = 0;

  while (i < alen && j < blen)
    {
      if (is_digit (a[i]) && is_digit (b[j]))
        {
          size_t za = i, zb = j;
          while (za < alen && a[za] == '0')
            za++;
          while (zb < blen && b[zb] == '0')
            zb++;
          size_t ea = za, eb = zb;
          while (ea < alen && is_digit (a[ea]))
            ea++;
          while (eb < blen && is_digit (b[eb]))
            eb++;

          /* Without leading zeros, a longer digit run is a bigger number. */
          size_t la = ea - za, lb = eb - zb;
          if (la != lb)
            return la < lb ? -1 : 1;
          int c = memcmp (a + za, b + zb, la);
          if (c != 0)
            return c < 0 ? -1 : 1;
          if (zeros_tiebreak == 0 && (za - i) != (zb - j))
            zeros_tiebreak = (za - i) < (zb - j) ? -1 : 1;
          i = ea;
          j = eb;
          continue;
        }

      /* A digit against a non-digit compares by byte. Digits are one contiguous byte range
       * with no other character inside it, so this stays consistent with numeric order. */
      unsigned char ca = (unsigned char) a[i], cb = (unsigned char) b[j];
      if (ca != cb)
        return ca < cb ? -1 : 1;
      i++;
      j++;
    }

  if (i < alen)
    return 1;
  if (j < blen)
    return -1;
  return zeros_tiebreak;
}
