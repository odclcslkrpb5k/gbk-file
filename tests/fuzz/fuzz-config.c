/* Input: a config file. Must never crash; must either load cleanly or report errors and
 * fall back to defaults; everything it accepts must be usable. */
#include "config.h"
#include "fuzz.h"

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GbkConfig) c = gbk_config_load_data ((const char *) data, size, errors);
  g_assert (c != nullptr && c->terminal != nullptr && c->terminal_run != nullptr);
  g_assert (gbk_command_uses (c->terminal_run, 'c'));
  if (errors->len > 0)
    g_assert (c->actions->len == 0);

  GbkTypeBuilder *b = gbk_type_builder_new ();
  gbk_config_apply_types (c, b);
  g_autoptr (GbkTypeTable) t = gbk_type_builder_finish (b);

  for (guint i = 0; i < c->actions->len; i++)
    {
      const GbkAction *a = g_ptr_array_index (c->actions, i);
      gbk_action_matches (a, "text/plain", false);
      gbk_action_matches (a, "inode/directory", true);
    }
  gbk_config_interpreter (c, "a.b.sh");
  return 0;
}
