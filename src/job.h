/* A file operation running on a worker thread, observed from the main thread.
 *
 * The worker never touches GTK. It sends progress, questions (conflict / error /
 * untrashable) and completion through a queue + eventfd. When it asks a question it waits
 * until the main thread calls gbk_job_answer() or gbk_job_cancel(). A running job keeps
 * itself alive (closing the window that started it doesn't stop it). */
#pragma once

#include "fileops.h"

typedef enum
{
  GBK_QUESTION_CONFLICT,     /* `path` (source) vs `other` (existing destination) */
  GBK_QUESTION_ERROR,        /* `other` is the error message; answer SKIP or CANCEL */
  GBK_QUESTION_UNTRASHABLE,  /* answer REPLACE to delete permanently, SKIP to keep */
} GbkQuestionKind;

typedef struct
{
  GbkQuestionKind kind;
  char *path;
  char *other;
  bool other_is_dir;
} GbkQuestion;

#define GBK_TYPE_JOB (gbk_job_get_type ())
G_DECLARE_FINAL_TYPE (GbkJob, gbk_job, GBK, JOB, GObject)

/* Signals:
 *   "progress"  ()
 *   "question"  (GbkQuestion *q)  answer with gbk_job_answer (now or later)
 *   "finished"  (GError *error)   nullptr on success (cancelled: G_IO_ERROR_CANCELLED) */

/* `sources`: absolute paths, copied. `dest_dir` for COPY/MOVE. */
[[nodiscard]] GbkJob *gbk_job_new (GbkOpKind kind, GPtrArray *sources, const char *dest_dir);
[[nodiscard]] GbkJob *gbk_job_new_rename (const char *path, const char *new_name);

void gbk_job_start (GbkJob *job);
void gbk_job_cancel (GbkJob *job);
void gbk_job_answer (GbkJob *job, GbkChoice choice, bool apply_all);

bool gbk_job_running (GbkJob *job);
GbkOpKind gbk_job_kind (GbkJob *job);          /* rename reports GBK_OP_MOVE */
const char *gbk_job_dest (GbkJob *job);        /* nullptr for trash/delete */
size_t gbk_job_n_sources (GbkJob *job);
double gbk_job_fraction (GbkJob *job);         /* 0..1, or -1 if unknown yet */
const char *gbk_job_current (GbkJob *job);     /* item being processed (display name) */
GPtrArray *gbk_job_created (GbkJob *job);      /* names created in dest (after finishing) */

/* Jobs still running (tests wait for 0). */
int gbk_job_live (void);
