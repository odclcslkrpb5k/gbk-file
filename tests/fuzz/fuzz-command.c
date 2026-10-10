/* Input: a command template. Whatever parses must expand without surprises: every
 * filename arrives as exactly one argument, byte for byte. */
#include "command.h"
#include "fuzz.h"

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  g_autofree char *tmpl = g_strndup ((const char *) data, size);
  g_autoptr (GbkCommand) c = gbk_command_parse (tmpl, "dfFc", nullptr);
  if (c == nullptr)
    return 0;

  const char *const files[] = { "/a/\"quoted\" 'x' $(y) %f %%", "/b/-rf" };
  const char *const runv[] = { "/bin/sh", "/c/script.sh" };
  GbkCommandArgs a = { .dir = "/d/%d dir", .files = files,
                       .n_files = gbk_command_uses (c, 'f') ? 1 : 2,
                       .run = runv, .n_run = 2 };
  g_auto (GStrv) argv = gbk_command_expand (c, &a);
  g_assert (argv[0] != nullptr && argv[0][0] != '\0');
  /* %F: each file must appear as its own, unmodified argument. */
  if (gbk_command_uses (c, 'F'))
    {
      bool found_a = false, found_b = false;
      for (size_t i = 0; argv[i] != nullptr; i++)
        {
          found_a |= g_str_equal (argv[i], files[0]);
          found_b |= g_str_equal (argv[i], files[1]);
        }
      g_assert (found_a && found_b);
    }
  return 0;
}
