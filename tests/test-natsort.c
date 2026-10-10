#include "natsort.h"

static int
cmp_names (const char *a, const char *b)
{
  size_t la, lb;
  g_autofree char *ka = gbk_sort_key_new (a, strlen (a), &la);
  g_autofree char *kb = gbk_sort_key_new (b, strlen (b), &lb);
  int r = gbk_natural_compare (ka, la, kb, lb);
  int back = gbk_natural_compare (kb, lb, ka, la);
  g_assert_cmpint (r, ==, -back);
  return r;
}

#define assert_before(a, b) g_assert_cmpint (cmp_names (a, b), <, 0)
#define assert_equiv(a, b) g_assert_cmpint (cmp_names (a, b), ==, 0)

static void
test_numeric (void)
{
  assert_before ("file2", "file10");
  assert_before ("file9", "file10");
  assert_before ("a1b2", "a1b10");
  assert_before ("1", "2");
  assert_before ("99999999999999999999999", "100000000000000000000000"); /* no overflow */
  assert_before ("v1.9", "v1.10");
}

static void
test_leading_zeros (void)
{
  assert_before ("file1", "file01");          /* equal value: fewer zeros first */
  assert_before ("file01", "file2");          /* value dominates zeros */
  assert_before ("x01y2", "x1y03");           /* first zero difference decides */
  assert_before ("0", "00");
}

static void
test_case_insensitive (void)
{
  assert_equiv ("README", "readme");
  assert_before ("apple", "Banana");
  assert_before ("Apple", "banana");
  assert_equiv ("ÉCOLE", "école");
}

static void
test_unicode (void)
{
  /* composed vs decomposed é sort together */
  assert_equiv ("caf\xc3\xa9", "cafe\xcc\x81");
  /* NFD: accented letters sort with their base letter */
  assert_before ("e", "\xc3\xa9");
  assert_before ("\xc3\xa9", "f");
  assert_before ("z", "\xd0\xb6");            /* Cyrillic zhe: code point order beyond ASCII */
}

static void
test_prefix_and_empty (void)
{
  assert_before ("", "a");
  assert_before ("a", "a1");
  assert_before ("a", "ab");
  assert_equiv ("", "");
}

static void
test_punctuation_vs_digits (void)
{
  assert_before ("a-1", "a1");                /* '-' 0x2d < digits */
  assert_before ("a1", "a_1");                /* '_' 0x5f > digits */
  assert_before ("a9", "a:");                 /* ':' 0x3a just above '9' */
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  g_test_add_func ("/natsort/numeric", test_numeric);
  g_test_add_func ("/natsort/leading-zeros", test_leading_zeros);
  g_test_add_func ("/natsort/case-insensitive", test_case_insensitive);
  g_test_add_func ("/natsort/unicode", test_unicode);
  g_test_add_func ("/natsort/prefix-and-empty", test_prefix_and_empty);
  g_test_add_func ("/natsort/punctuation-vs-digits", test_punctuation_vs_digits);
  return g_test_run ();
}
