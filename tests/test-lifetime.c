#include "lifetime.h"

/* A minimal GObject with one signal, "ping", to act as the emitting instance. */
#define TEST_TYPE_PINGER (test_pinger_get_type ())
G_DECLARE_FINAL_TYPE (TestPinger, test_pinger, TEST, PINGER, GObject)
struct _TestPinger { GObject parent_instance; };
G_DEFINE_FINAL_TYPE (TestPinger, test_pinger, G_TYPE_OBJECT)
static guint ping_signal;
static void test_pinger_class_init (TestPingerClass *klass)
{
  ping_signal = g_signal_new ("ping", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                              0, nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
}
static void test_pinger_init (TestPinger *self G_GNUC_UNUSED) {}

static void
ping (TestPinger *p)
{
  g_signal_emit (p, ping_signal, 0);
}

static void
count_ping (TestPinger *p G_GNUC_UNUSED, int *counter)
{
  (*counter)++;
}

static bool
count_source (void *data)
{
  int *counter = data;
  (*counter)++;
  return true;
}

static bool
count_source_once (void *data)
{
  int *counter = data;
  (*counter)++;
  return false;
}

static void
iterate (int times)
{
  for (int i = 0; i < times; i++)
    g_main_context_iteration (nullptr, false);
}

/* ---- signals ---- */

static void
test_signal_fires (void)
{
  int calls = 0;
  GObject *owner = g_object_new (G_TYPE_OBJECT, nullptr);
  TestPinger *p = g_object_new (TEST_TYPE_PINGER, nullptr);

  g_assert_cmpuint (gbk_connect (owner, p, "ping", G_CALLBACK (count_ping), &calls), !=, 0);
  g_assert_cmpint (gbk_lifetime_live (), ==, 1);
  ping (p);
  ping (p);
  g_assert_cmpint (calls, ==, 2);

  g_object_unref (owner);
  g_object_unref (p);
  g_assert_cmpint (gbk_lifetime_live (), ==, 0);
}

static void
test_signal_owner_disposed_first (void)
{
  int calls = 0;
  GObject *owner = g_object_new (G_TYPE_OBJECT, nullptr);
  TestPinger *p = g_object_new (TEST_TYPE_PINGER, nullptr);

  gbk_connect (owner, p, "ping", G_CALLBACK (count_ping), &calls);
  g_object_unref (owner);                 /* owner gone: handler must be gone too */
  g_assert_cmpint (gbk_lifetime_live (), ==, 0);
  ping (p);
  g_assert_cmpint (calls, ==, 0);

  g_object_unref (p);
}

static void
test_signal_owner_run_dispose (void)
{
  /* gtk_widget_destroy() disposes while other refs still exist; must behave the same. */
  int calls = 0;
  GObject *owner = g_object_new (G_TYPE_OBJECT, nullptr);
  TestPinger *p = g_object_new (TEST_TYPE_PINGER, nullptr);

  gbk_connect (owner, p, "ping", G_CALLBACK (count_ping), &calls);
  g_object_run_dispose (owner);
  ping (p);
  g_assert_cmpint (calls, ==, 0);
  g_assert_cmpint (gbk_lifetime_live (), ==, 0);

  g_object_unref (owner);
  g_object_unref (p);
}

static void
test_signal_instance_finalized_first (void)
{
  int calls = 0;
  GObject *owner = g_object_new (G_TYPE_OBJECT, nullptr);
  TestPinger *p = g_object_new (TEST_TYPE_PINGER, nullptr);

  gbk_connect (owner, p, "ping", G_CALLBACK (count_ping), &calls);
  g_object_unref (p);
  g_assert_cmpint (gbk_lifetime_live (), ==, 0);
  g_object_unref (owner);                 /* must not touch the dead instance */
  g_assert_cmpint (gbk_lifetime_live (), ==, 0);
}

/* ---- sources ---- */

static void
test_source_runs_until_false (void)
{
  int calls = 0;
  GObject *owner = g_object_new (G_TYPE_OBJECT, nullptr);

  gbk_idle_add (owner, count_source_once, &calls);
  g_assert_cmpint (gbk_lifetime_live (), ==, 1);
  iterate (5);
  g_assert_cmpint (calls, ==, 1);
  g_assert_cmpint (gbk_lifetime_live (), ==, 0);

  g_object_unref (owner);
}

static void
test_source_owner_disposed_first (void)
{
  int calls = 0;
  GObject *owner = g_object_new (G_TYPE_OBJECT, nullptr);

  gbk_idle_add (owner, count_source, &calls);
  gbk_timeout_add (owner, 0, count_source, &calls);
  g_assert_cmpint (gbk_lifetime_live (), ==, 2);
  g_object_unref (owner);
  g_assert_cmpint (gbk_lifetime_live (), ==, 0);
  iterate (5);
  g_assert_cmpint (calls, ==, 0);
}

static void
test_source_owner_run_dispose (void)
{
  int calls = 0;
  GObject *owner = g_object_new (G_TYPE_OBJECT, nullptr);

  gbk_idle_add (owner, count_source, &calls);
  g_object_run_dispose (owner);
  iterate (5);
  g_assert_cmpint (calls, ==, 0);
  g_assert_cmpint (gbk_lifetime_live (), ==, 0);

  g_object_unref (owner);
}

typedef struct
{
  GObject *owner;   /* the only ref; the callback drops it */
  int calls;
} SelfDestruct;

static bool
drop_owner_and_continue (void *data)
{
  SelfDestruct *s = data;
  s->calls++;
  g_clear_object (&s->owner);
  return true;   /* asks to continue, but the owner is gone: must not run again */
}

static void
test_source_callback_drops_owner (void)
{
  SelfDestruct s = { .owner = g_object_new (G_TYPE_OBJECT, nullptr) };

  gbk_idle_add (s.owner, drop_owner_and_continue, &s);
  iterate (5);
  g_assert_cmpint (s.calls, ==, 1);
  g_assert_null (s.owner);
  g_assert_cmpint (gbk_lifetime_live (), ==, 0);
}

static void
test_source_cancel (void)
{
  int calls = 0;
  GObject *owner = g_object_new (G_TYPE_OBJECT, nullptr);

  guint id = gbk_timeout_add (owner, 0, count_source, &calls);
  gbk_source_cancel (&id);
  g_assert_cmpuint (id, ==, 0);
  g_assert_cmpint (gbk_lifetime_live (), ==, 0);
  iterate (5);
  g_assert_cmpint (calls, ==, 0);

  gbk_source_cancel (&id);                /* zero id: no-op */

  guint stale = gbk_idle_add (owner, count_source_once, &calls);
  iterate (5);                            /* finished on its own; id is now stale */
  g_assert_cmpint (calls, ==, 1);
  gbk_source_cancel (&stale);             /* must not raise a critical */
  g_assert_cmpuint (stale, ==, 0);

  g_object_unref (owner);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);

  g_test_add_func ("/lifetime/signal/fires", test_signal_fires);
  g_test_add_func ("/lifetime/signal/owner-disposed-first", test_signal_owner_disposed_first);
  g_test_add_func ("/lifetime/signal/owner-run-dispose", test_signal_owner_run_dispose);
  g_test_add_func ("/lifetime/signal/instance-finalized-first", test_signal_instance_finalized_first);
  g_test_add_func ("/lifetime/source/runs-until-false", test_source_runs_until_false);
  g_test_add_func ("/lifetime/source/owner-disposed-first", test_source_owner_disposed_first);
  g_test_add_func ("/lifetime/source/owner-run-dispose", test_source_owner_run_dispose);
  g_test_add_func ("/lifetime/source/callback-drops-owner", test_source_callback_drops_owner);
  g_test_add_func ("/lifetime/source/cancel", test_source_cancel);

  return g_test_run ();
}
