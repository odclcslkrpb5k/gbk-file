/* Input: arbitrary bytes as an inotify read buffer. */
#include "inotify-parse.h"
#include "fuzz.h"

static void
on_event (int wd, uint32_t mask, const char *name, size_t name_len, void *data)
{
  size_t *count = data;
  (void) wd;
  (void) mask;
  if (name != nullptr)
    {
      g_assert (name_len > 0);
      g_assert (name[name_len] == '\0');       /* in bounds (ASan) and terminated */
      g_assert (strlen (name) == name_len);
    }
  (*count)++;
}

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  size_t count = 0;
  gbk_inotify_parse (data, size, on_event, &count);
  g_assert (count <= size / sizeof (struct inotify_event));
  return 0;
}
