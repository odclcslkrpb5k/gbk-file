/* Replays files (or every file in directories) through LLVMFuzzerTestOneInput. Used in
 * non-fuzzing builds so seed corpora run under the sanitizers as ordinary tests. */
#include "fuzz.h"

static int
run_file (const char *path)
{
  g_autofree char *data = nullptr;
  gsize len = 0;
  g_autoptr (GError) error = nullptr;
  if (!g_file_get_contents (path, &data, &len, &error))
    {
      fprintf (stderr, "%s\n", error->message);
      return 1;
    }
  LLVMFuzzerTestOneInput ((const uint8_t *) data, len);
  return 0;
}

int
main (int argc, char **argv)
{
  int files = 0;
  for (int i = 1; i < argc; i++)
    {
      if (!g_file_test (argv[i], G_FILE_TEST_IS_DIR))
        {
          if (run_file (argv[i]) != 0)
            return 1;
          files++;
          continue;
        }
      g_autoptr (GDir) dir = g_dir_open (argv[i], 0, nullptr);
      if (dir == nullptr)
        return 1;
      const char *name;
      while ((name = g_dir_read_name (dir)) != nullptr)
        {
          g_autofree char *path = g_build_filename (argv[i], name, nullptr);
          if (run_file (path) != 0)
            return 1;
          files++;
        }
    }
  /* An empty corpus would make this test pass vacuously. */
  if (files == 0)
    {
      fprintf (stderr, "no seed inputs found\n");
      return 1;
    }
  printf ("replayed %d inputs\n", files);
  return 0;
}
