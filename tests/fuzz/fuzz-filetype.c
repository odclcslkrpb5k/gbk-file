/* Input: globs2 data. Every line of it is also looked up as a filename. */
#include "filetype.h"
#include "fuzz.h"

static void
check (GbkTypeTable *t, const char *name, size_t len, GbkFileKind kind, bool exec)
{
  GbkTypeId id = gbk_type_table_lookup (t, name, len, kind, exec);
  g_assert (id < gbk_type_table_size (t));
  g_assert (gbk_type_table_mime (t, id)[0] != '\0');
  g_assert (gbk_type_table_icon (t, id)[0] != '\0');
  g_assert (gbk_type_table_generic_icon (t, id)[0] != '\0');
}

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  const char *text = (const char *) data;
  GbkTypeBuilder *b = gbk_type_builder_new ();
  gbk_type_builder_load_globs2 (b, text, size);
  gbk_type_builder_load_generic_icons (b, text, size);
  g_autoptr (GbkTypeTable) t = gbk_type_builder_finish (b);

  const char *p = text;
  const char *end = text + size;
  while (p < end)
    {
      const char *nl = memchr (p, '\n', (size_t) (end - p));
      size_t len = (size_t) ((nl != nullptr ? nl : end) - p);
      check (t, p, len, GBK_KIND_REGULAR, false);
      check (t, p, len, GBK_KIND_REGULAR, true);
      check (t, p, len, (GbkFileKind) (len % 7), false);
      p += len + 1;
    }
  return 0;
}
