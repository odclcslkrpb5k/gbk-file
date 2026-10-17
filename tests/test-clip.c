#include "cliptext.h"

static GPtrArray *
list (const char *first, ...)
{
  GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
  va_list ap;
  va_start (ap, first);
  for (const char *p = first; p != nullptr; p = va_arg (ap, const char *))
    g_ptr_array_add (a, g_strdup (p));
  va_end (ap);
  return a;
}

static void
test_round_trip (void)
{
  g_autoptr (GPtrArray) paths
      = list ("/home/u/a b.txt", "/home/u/\xff\xfe raw", "/x/%25", (const char *) nullptr);
  for (int cut = 0; cut < 2; cut++)
    {
      g_autofree char *g = gbk_clip_format_gnome (paths, cut);
      g_assert_true (g_str_has_prefix (g, cut ? "cut\nfile:///" : "copy\nfile:///"));
      bool was_cut;
      g_autoptr (GPtrArray) back = gbk_clip_parse (g, strlen (g), true, &was_cut);
      g_assert_cmpint (was_cut, ==, cut);
      g_assert_cmpuint (back->len, ==, 3);
      for (guint i = 0; i < 3; i++)
        g_assert_cmpstr (g_ptr_array_index (back, i), ==, g_ptr_array_index (paths, i));
    }
  g_autofree char *u = gbk_clip_format_uri_list (paths);
  g_assert_true (g_str_has_suffix (u, "\r\n"));
  bool cut;
  g_autoptr (GPtrArray) back = gbk_clip_parse (u, strlen (u), false, &cut);
  g_assert_false (cut);
  g_assert_cmpuint (back->len, ==, 3);
}

static void
test_foreign (void)
{
  const char data[] = "# from a browser\r\nhttp://example.com/\r\nfile:///ok\r\n"
                      "not a uri\r\nfile://remotehost/x\r\nfile:///also%20ok\r\n";
  bool cut;
  g_autoptr (GPtrArray) p = gbk_clip_parse (data, strlen (data), false, &cut);
  g_assert_cmpuint (p->len, ==, 2);
  g_assert_cmpstr (g_ptr_array_index (p, 0), ==, "/ok");
  g_assert_cmpstr (g_ptr_array_index (p, 1), ==, "/also ok");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  g_test_add_func ("/clip/round-trip", test_round_trip);
  g_test_add_func ("/clip/foreign", test_foreign);
  return g_test_run ();
}
