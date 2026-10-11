#include "job.h"
#include "lifetime.h"

#define PROGRESS_INTERVAL_US (50 * 1000)

typedef enum
{
  MSG_PROGRESS,
  MSG_QUESTION,
  MSG_FINISHED,
} MsgType;

typedef struct
{
  MsgType type;
  uint64_t done, total;
  char *current;
  GbkQuestion q;
  GError *error;
  GPtrArray *created;
} Msg;

/* Shared between the main thread and the worker. */
typedef struct
{
  gatomicrefcount rc;
  GAsyncQueue *queue;      /* Msg* */
  int wake_fd;
  int cancelled;           /* atomic */

  GMutex lock;             /* guards the answer */
  GCond cond;
  bool answered;
  GbkChoice answer;
  bool answer_all;

  GbkOpKind kind;
  bool rename;
  GPtrArray *sources;
  char *dest;
  char *new_name;
  int64_t last_progress;   /* worker only */
} Shared;

struct _GbkJob
{
  GObject parent_instance;
  Shared *s;
  guint watch;
  bool running;
  bool waiting;            /* a question is outstanding */
  uint64_t done, total;
  char *current;
  GPtrArray *created;
};

G_DEFINE_FINAL_TYPE (GbkJob, gbk_job, G_TYPE_OBJECT)

enum { SIGNAL_PROGRESS, SIGNAL_QUESTION, SIGNAL_FINISHED, N_SIGNALS };
static guint signals[N_SIGNALS];
static int live_jobs;

int
gbk_job_live (void)
{
  return g_atomic_int_get (&live_jobs);
}

static void
msg_free (Msg *m)
{
  g_free (m->current);
  g_free (m->q.path);
  g_free (m->q.other);
  g_clear_error (&m->error);
  g_clear_pointer (&m->created, g_ptr_array_unref);
  g_free (m);
}

static void
shared_unref (Shared *s)
{
  if (!g_atomic_ref_count_dec (&s->rc))
    return;
  g_async_queue_unref (s->queue);
  close (s->wake_fd);
  g_mutex_clear (&s->lock);
  g_cond_clear (&s->cond);
  g_ptr_array_unref (s->sources);
  g_free (s->dest);
  g_free (s->new_name);
  g_free (s);
}

static void
send (Shared *s, Msg *m)
{
  g_async_queue_push (s->queue, m);
  uint64_t one = 1;
  while (write (s->wake_fd, &one, sizeof one) < 0 && errno == EINTR)
    ;
}

static bool
is_cancelled (Shared *s)
{
  return g_atomic_int_get (&s->cancelled) != 0;
}

/* ---- worker side ---- */

/* Ask the main thread and block until it answers (or the job is cancelled). */
static GbkChoice
ask (Shared *s, GbkQuestionKind kind, const char *path, const char *other, bool other_is_dir,
     bool *apply_all)
{
  g_mutex_lock (&s->lock);
  s->answered = false;
  g_mutex_unlock (&s->lock);

  Msg *m = g_new0 (Msg, 1);
  m->type = MSG_QUESTION;
  m->q.kind = kind;
  m->q.path = g_strdup (path);
  m->q.other = g_strdup (other);
  m->q.other_is_dir = other_is_dir;
  send (s, m);

  g_mutex_lock (&s->lock);
  while (!s->answered && !is_cancelled (s))
    g_cond_wait (&s->cond, &s->lock);
  GbkChoice choice = s->answered ? s->answer : GBK_CHOICE_CANCEL;
  if (apply_all != nullptr)
    *apply_all = s->answered && s->answer_all;
  g_mutex_unlock (&s->lock);
  return is_cancelled (s) ? GBK_CHOICE_CANCEL : choice;
}

static GbkChoice
hook_conflict (const char *src, const char *dst, bool dst_is_dir, bool *apply_all, void *data)
{
  return ask (data, GBK_QUESTION_CONFLICT, src, dst, dst_is_dir, apply_all);
}

static GbkChoice
hook_error (const char *path, const GError *error, void *data)
{
  return ask (data, GBK_QUESTION_ERROR, path, error->message, false, nullptr);
}

static bool
hook_untrashable (const char *path, void *data)
{
  return ask (data, GBK_QUESTION_UNTRASHABLE, path, nullptr, false, nullptr) == GBK_CHOICE_REPLACE;
}

static void
hook_progress (uint64_t done, uint64_t total, const char *current, void *data)
{
  Shared *s = data;
  int64_t now = g_get_monotonic_time ();
  if (now - s->last_progress < PROGRESS_INTERVAL_US)
    return;
  s->last_progress = now;
  Msg *m = g_new0 (Msg, 1);
  m->type = MSG_PROGRESS;
  m->done = done;
  m->total = total;
  m->current = g_filename_display_basename (current);
  send (s, m);
}

static bool
hook_cancelled (void *data)
{
  return is_cancelled (data);
}

static void *
worker (void *data)
{
  Shared *s = data;
  GbkOpsHooks hooks = { hook_conflict, hook_error, hook_untrashable, hook_progress,
                        hook_cancelled, s };
  Msg *done = g_new0 (Msg, 1);
  done->type = MSG_FINISHED;
  done->created = g_ptr_array_new_with_free_func (g_free);
  if (s->rename)
    {
      if (gbk_ops_rename (g_ptr_array_index (s->sources, 0), s->new_name, &done->error))
        g_ptr_array_add (done->created, g_strdup (s->new_name));
    }
  else
    gbk_ops_run (s->kind, s->sources, s->dest, &hooks, done->created, &done->error);
  send (s, done);
  shared_unref (s);
  return nullptr;
}

/* ---- main thread side ---- */

static void
finish (GbkJob *job, Msg *m)
{
  job->running = false;
  job->waiting = false;
  g_clear_pointer (&job->created, g_ptr_array_unref);
  job->created = g_steal_pointer (&m->created);
  gbk_source_cancel (&job->watch);
  g_signal_emit (job, signals[SIGNAL_FINISHED], 0, m->error);
  g_atomic_int_dec_and_test (&live_jobs);
  g_object_unref (job);        /* the running job's self-reference */
}

static bool
on_wake (int fd G_GNUC_UNUSED, GIOCondition cond G_GNUC_UNUSED, void *data)
{
  GbkJob *job = data;
  uint64_t v;
  while (read (job->s->wake_fd, &v, sizeof v) < 0 && errno == EINTR)
    ;
  /* The worker blocks on each question, so at most one is ever queued. */
  Msg *m;
  while (job->running && (m = g_async_queue_try_pop (job->s->queue)) != nullptr)
    {
      switch (m->type)
        {
        case MSG_PROGRESS:
          job->done = m->done;
          job->total = m->total;
          g_free (job->current);
          job->current = g_steal_pointer (&m->current);
          g_signal_emit (job, signals[SIGNAL_PROGRESS], 0);
          break;
        case MSG_QUESTION:
          job->waiting = true;
          g_signal_emit (job, signals[SIGNAL_QUESTION], 0, &m->q);
          break;
        case MSG_FINISHED:
          {
            /* finish() may drop the last reference: keep `job` valid until we return. */
            g_autoptr (GbkJob) hold = g_object_ref (job);
            finish (job, m);
            msg_free (m);
            return false;
          }
        default:
          gbk_fatal ("bad job message %d", (int) m->type);
        }
      msg_free (m);
    }
  return job->running;
}

void
gbk_job_answer (GbkJob *job, GbkChoice choice, bool apply_all)
{
  if (!job->waiting)
    return;              /* stale answer (e.g. dialog closed after cancel) */
  job->waiting = false;
  g_mutex_lock (&job->s->lock);
  job->s->answer = choice;
  job->s->answer_all = apply_all;
  job->s->answered = true;
  g_cond_broadcast (&job->s->cond);
  g_mutex_unlock (&job->s->lock);
}

void
gbk_job_cancel (GbkJob *job)
{
  g_atomic_int_set (&job->s->cancelled, 1);
  g_mutex_lock (&job->s->lock);
  g_cond_broadcast (&job->s->cond);     /* wake a worker waiting for an answer */
  g_mutex_unlock (&job->s->lock);
}

void
gbk_job_start (GbkJob *job)
{
  g_assert (!job->running && job->watch == 0);
  job->running = true;
  g_object_ref (job);                   /* released in finish() */
  g_atomic_int_inc (&live_jobs);
  job->watch = gbk_fd_watch (G_OBJECT (job), job->s->wake_fd, G_IO_IN, on_wake, job);
  g_atomic_ref_count_inc (&job->s->rc);
  g_autoptr (GError) error = nullptr;
  GThread *t = g_thread_try_new ("gbk-job", worker, job->s, &error);
  if (t == nullptr)
    {
      shared_unref (job->s);
      Msg *m = g_new0 (Msg, 1);
      m->type = MSG_FINISHED;
      m->error = g_steal_pointer (&error);
      m->created = g_ptr_array_new_with_free_func (g_free);
      send (job->s, m);                 /* delivered via the normal path */
      return;
    }
  g_thread_unref (t);
}

static void
gbk_job_finalize (GObject *o)
{
  GbkJob *job = GBK_JOB (o);
  g_assert (!job->running);
  shared_unref (job->s);
  g_free (job->current);
  g_clear_pointer (&job->created, g_ptr_array_unref);
  G_OBJECT_CLASS (gbk_job_parent_class)->finalize (o);
}

static void
gbk_job_class_init (GbkJobClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = gbk_job_finalize;
  signals[SIGNAL_PROGRESS] = g_signal_new ("progress", G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
                                           G_TYPE_NONE, 0);
  signals[SIGNAL_QUESTION] = g_signal_new ("question", G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
                                           G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[SIGNAL_FINISHED] = g_signal_new ("finished", G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
                                           G_TYPE_NONE, 1, G_TYPE_ERROR);
}

static void
gbk_job_init (GbkJob *job G_GNUC_UNUSED)
{
}

static GbkJob *
job_new (GbkOpKind kind, GPtrArray *sources, const char *dest)
{
  GbkJob *job = g_object_new (GBK_TYPE_JOB, nullptr);
  Shared *s = g_new0 (Shared, 1);
  g_atomic_ref_count_init (&s->rc);
  s->queue = g_async_queue_new_full ((GDestroyNotify) msg_free);
  s->wake_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (s->wake_fd < 0)
    gbk_fatal ("eventfd: %s", g_strerror (errno));
  g_mutex_init (&s->lock);
  g_cond_init (&s->cond);
  s->kind = kind;
  s->sources = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < sources->len; i++)
    g_ptr_array_add (s->sources, g_strdup (g_ptr_array_index (sources, i)));
  s->dest = g_strdup (dest);
  job->s = s;
  job->total = 0;
  return job;
}

GbkJob *
gbk_job_new (GbkOpKind kind, GPtrArray *sources, const char *dest_dir)
{
  return job_new (kind, sources, dest_dir);
}

GbkJob *
gbk_job_new_rename (const char *path, const char *new_name)
{
  g_autoptr (GPtrArray) one = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (one, g_strdup (path));
  g_autofree char *dir = g_path_get_dirname (path);
  GbkJob *job = job_new (GBK_OP_MOVE, one, dir);
  job->s->rename = true;
  job->s->new_name = g_strdup (new_name);
  return job;
}

bool gbk_job_running (GbkJob *job) { return job->running; }
GbkOpKind gbk_job_kind (GbkJob *job) { return job->s->kind; }
const char *gbk_job_dest (GbkJob *job) { return job->s->dest; }
size_t gbk_job_n_sources (GbkJob *job) { return job->s->sources->len; }
const char *gbk_job_current (GbkJob *job) { return job->current != nullptr ? job->current : ""; }
GPtrArray *gbk_job_created (GbkJob *job) { return job->created; }

double
gbk_job_fraction (GbkJob *job)
{
  if (job->total == 0)
    return -1;
  return CLAMP ((double) job->done / (double) job->total, 0.0, 1.0);
}
