/* Input: three names separated by '\n'. Checks the ordering axioms that sorting relies on:
 * reflexivity, antisymmetry, and transitivity. A violation means qsort may misbehave. */
#include "natsort.h"
#include "fuzz.h"

static int
sign (int v)
{
  return (v > 0) - (v < 0);
}

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  /* Keys come from display names, which are always valid UTF-8. */
  g_autofree char *text = g_utf8_make_valid ((const char *) data, (gssize) size);
  g_auto (GStrv) parts = g_strsplit (text, "\n", 3);
  const char *names[3] = { "", "", "" };
  for (size_t i = 0; i < 3 && parts[i] != nullptr; i++)
    names[i] = parts[i];

  char *k[3];
  size_t l[3];
  for (size_t i = 0; i < 3; i++)
    k[i] = gbk_sort_key_new (names[i], strlen (names[i]), &l[i]);

  int c[3][3];
  for (size_t i = 0; i < 3; i++)
    for (size_t j = 0; j < 3; j++)
      c[i][j] = sign (gbk_natural_compare (k[i], l[i], k[j], l[j]));

  for (size_t i = 0; i < 3; i++)
    {
      g_assert (c[i][i] == 0);
      for (size_t j = 0; j < 3; j++)
        {
          g_assert (c[i][j] == -c[j][i]);
          for (size_t m = 0; m < 3; m++)
            if (c[i][j] <= 0 && c[j][m] <= 0)
              g_assert (c[i][m] <= 0);
        }
    }

  for (size_t i = 0; i < 3; i++)
    g_free (k[i]);
  return 0;
}
