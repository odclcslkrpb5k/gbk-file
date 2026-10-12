/* Jobs: worker thread + questions + cancellation, driven through the main loop. */
#include "job.h"
#include "lifetime.h"

static char *root;

typedef struct
{
  bool finished;
  GError *error;
  int questions;
  GbkQuestionKind last_kind;
  GbkChoice answer;
  bool answer_later;      /* answer from an idle callback, not inside the signal */
  bool cancel_on_question;
  GbkJob *job;            /* not owned */
} Watch;

static bool
answer_idle (void *data)
{
  Watch *w = data;
  gbk_job_answer (w->job, w->answer, false);
  return false;
}

static void
on_question (GbkJob *job, GbkQuestion *q, Watch *w)
{
  w->questions++;
  w->last_kind = q->kind;
  g_assert_nonnull (q->path);
  if (w->cancel_on_question)
    gbk_job_cancel (job);
  else if (w->answer_later)
    gbk_idle_add (G_OBJECT (job), answer_idle, w);
  else
    gbk_job_answer (job, w->answer, false);
}

static void
on_finished (GbkJob *job G_GNUC_UNUSED, GError *error, Watch *w)
{
  w->finished = true;
  w->error = error != nullptr ? g_error_copy (error) : nullptr;
}

static void
wait_finished (Watch *w)
{
  int64_t deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
  while (!w->finished && g_get_monotonic_time () < deadline)
    g_main_context_iteration (nullptr, true);
  g_assert_true (w->finished);
}

static GbkJob *
start (GbkOpKind kind, const char *src_rel, const char *dest_rel, Watch *w, GObject *owner)
{
  g_autoptr (GPtrArray) src = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (src, g_build_filename (root, src_rel, nullptr));
  g_autofree char *dest = dest_rel != nullptr ? g_build_filename (root, dest_rel, nullptr) : nullptr;
  GbkJob *job = gbk_job_new (kind, src, dest);
  w->job = job;
  gbk_connect (owner, job, "question", G_CALLBACK (on_question), w);
  gbk_connect (owner, job, "finished", G_CALLBACK (on_finished), w);
  gbk_job_start (job);
  return job;
}

static void
put (const char *rel, const char *content)
{
  g_autofree char *p = g_build_filename (root, rel, nullptr);
  g_autofree char *d = g_path_get_dirname (p);
  g_mkdir_with_parents (d, 0755);
  g_assert_true (g_file_set_contents (p, content, -1, nullptr));
}

static bool
exists (const char *rel)
{
  g_autofree char *p = g_build_filename (root, rel, nullptr);
  return g_file_test (p, G_FILE_TEST_EXISTS);
}

static void
test_copy (void)
{
  put ("a/f.txt", "x");
  put ("b/keep", "");
  g_autoptr (GObject) owner = g_object_new (G_TYPE_OBJECT, nullptr);
  Watch w = { 0 };
  g_autoptr (GbkJob) job = start (GBK_OP_COPY, "a/f.txt", "b", &w, owner);
  wait_finished (&w);
  g_assert_null (w.error);
  g_assert_true (exists ("b/f.txt"));
  g_assert_cmpuint (gbk_job_created (job)->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (gbk_job_created (job), 0), ==, "f.txt");
  g_assert_false (gbk_job_running (job));
}

static void
test_conflict_answers (void)
{
  put ("c/f.txt", "new");
  put ("d/f.txt", "old");
  g_autoptr (GObject) owner = g_object_new (G_TYPE_OBJECT, nullptr);

  Watch now = { .answer = GBK_CHOICE_KEEP_BOTH };
  g_autoptr (GbkJob) j1 = start (GBK_OP_COPY, "c/f.txt", "d", &now, owner);
  wait_finished (&now);
  g_assert_cmpint (now.questions, ==, 1);
  g_assert_cmpint (now.last_kind, ==, GBK_QUESTION_CONFLICT);
  g_assert_true (exists ("d/f (1).txt"));

  /* Answered later, from the main loop: the worker must wait for it. */
  Watch later = { .answer = GBK_CHOICE_SKIP, .answer_later = true };
  g_autoptr (GbkJob) j2 = start (GBK_OP_COPY, "c/f.txt", "d", &later, owner);
  wait_finished (&later);
  g_assert_null (later.error);
  g_assert_false (exists ("d/f (2).txt"));
}

static void
test_cancel_while_asking (void)
{
  put ("e/f.txt", "new");
  put ("g/f.txt", "old");
  g_autoptr (GObject) owner = g_object_new (G_TYPE_OBJECT, nullptr);
  Watch w = { .cancel_on_question = true };
  g_autoptr (GbkJob) job = start (GBK_OP_COPY, "e/f.txt", "g", &w, owner);
  wait_finished (&w);
  g_assert_error (w.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&w.error);
}

static bool
no_jobs (void)
{
  int64_t deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
  while (gbk_job_live () != 0 && g_get_monotonic_time () < deadline)
    g_main_context_iteration (nullptr, true);
  return gbk_job_live () == 0;
}

static void
test_outlives_creator (void)
{
  /* Start, drop every reference we have: the job finishes anyway. */
  put ("h/f.txt", "x");
  put ("i/keep", "");
  g_autoptr (GPtrArray) src = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (src, g_build_filename (root, "h/f.txt", nullptr));
  g_autofree char *dest = g_build_filename (root, "i", nullptr);
  GbkJob *job = gbk_job_new (GBK_OP_MOVE, src, dest);
  gbk_job_start (job);
  g_object_unref (job);
  g_assert_true (no_jobs ());
  g_assert_true (exists ("i/f.txt"));
  g_assert_false (exists ("h/f.txt"));
}

static void
test_rename_error (void)
{
  put ("j/a", "");
  put ("j/b", "");
  g_autoptr (GObject) owner = g_object_new (G_TYPE_OBJECT, nullptr);
  g_autofree char *a = g_build_filename (root, "j/a", nullptr);
  Watch w = { 0 };
  g_autoptr (GbkJob) job = gbk_job_new_rename (a, "b");
  gbk_connect (owner, job, "finished", G_CALLBACK (on_finished), &w);
  gbk_job_start (job);
  wait_finished (&w);
  g_assert_error (w.error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error (&w.error);
}

/* Start `job` with a watch owned by `owner` and wait for it. */
static void
run_job (GbkJob *job, Watch *w, GObject *owner)
{
  w->job = job;
  gbk_connect (owner, job, "question", G_CALLBACK (on_question), w);
  gbk_connect (owner, job, "finished", G_CALLBACK (on_finished), w);
  gbk_job_start (job);
  wait_finished (w);
}

static void
test_create_info_executable (void)
{
  g_autoptr (GObject) owner = g_object_new (G_TYPE_OBJECT, nullptr);
  g_autofree char *dir = g_build_filename (root, "k", nullptr);
  g_mkdir_with_parents (dir, 0755);

  /* Create: reports the name it created, in its destination. */
  Watch w1 = { 0 };
  g_autoptr (GbkJob) create = gbk_job_new_create (dir, "New Folder", true);
  g_assert_cmpint (gbk_job_kind (create), ==, GBK_JOB_CREATE_FOLDER);
  run_job (create, &w1, owner);
  g_assert_no_error (w1.error);
  g_assert_cmpstr (gbk_job_dest (create), ==, dir);
  g_assert_cmpuint (gbk_job_created (create)->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (gbk_job_created (create), 0), ==, "New Folder");
  g_assert_true (exists ("k/New Folder"));

  /* Creating it again fails without asking anything. */
  Watch w2 = { 0 };
  g_autoptr (GbkJob) again = gbk_job_new_create (dir, "New Folder", true);
  run_job (again, &w2, owner);
  g_assert_error (w2.error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_assert_cmpint (w2.questions, ==, 0);
  g_clear_error (&w2.error);

  /* Executable on two files; one of them a folder: asked once (Skip), the other done. */
  put ("k/a.sh", "");
  g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (paths, g_build_filename (root, "k/New Folder", nullptr));
  g_ptr_array_add (paths, g_build_filename (root, "k/a.sh", nullptr));
  Watch w3 = { .answer = GBK_CHOICE_SKIP };
  g_autoptr (GbkJob) exec = gbk_job_new_executable (paths, true);
  run_job (exec, &w3, owner);
  g_assert_no_error (w3.error);
  g_assert_cmpint (w3.questions, ==, 1);
  g_assert_cmpint (w3.last_kind, ==, GBK_QUESTION_ERROR);
  g_assert_true (g_file_test (g_ptr_array_index (paths, 1), G_FILE_TEST_IS_EXECUTABLE));

  /* Info: the result is there once it's finished. */
  Watch w4 = { 0 };
  g_autoptr (GbkJob) info = gbk_job_new_info (g_ptr_array_index (paths, 1));
  g_assert_null (gbk_job_info (info));
  run_job (info, &w4, owner);
  g_assert_no_error (w4.error);
  g_assert_nonnull (gbk_job_info (info));
  g_assert_true (S_ISREG (gbk_job_info (info)->mode));
  g_assert_cmpint (gbk_job_info (info)->mode & 0100, ==, 0100);

  /* Permissions. */
  Watch w5 = { 0 };
  g_autoptr (GbkJob) perm = gbk_job_new_permissions (g_ptr_array_index (paths, 1), 0600);
  run_job (perm, &w5, owner);
  g_assert_no_error (w5.error);
  g_assert_false (g_file_test (g_ptr_array_index (paths, 1), G_FILE_TEST_IS_EXECUTABLE));
}

int
main (int argc, char **argv)
{
  root = g_dir_make_tmp ("gbk-job-XXXXXX", nullptr);
  g_test_init (&argc, &argv, nullptr);
  g_test_add_func ("/job/copy", test_copy);
  g_test_add_func ("/job/conflict-answers", test_conflict_answers);
  g_test_add_func ("/job/cancel-while-asking", test_cancel_while_asking);
  g_test_add_func ("/job/outlives-creator", test_outlives_creator);
  g_test_add_func ("/job/rename-error", test_rename_error);
  g_test_add_func ("/job/create-info-executable", test_create_info_executable);
  int status = g_test_run ();
  if (!no_jobs () || gbk_lifetime_live () != 0)
    gbk_fatal ("jobs or registrations leaked");
  g_free (root);
  return status;
}
