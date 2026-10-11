/* One inotify instance for the whole process, shared by every scanner.
 *
 * Linux limits inotify instances per user (fs.inotify.max_user_instances, often 128, shared
 * with the whole desktop); one per open folder could exhaust it and silently stop live
 * updates. Instead a single hub thread reads one inotify fd and routes events, by watch
 * descriptor, to subscribers. Each subscriber has its own bounded buffer and an eventfd.
 *
 * Several subscribers may watch the same inode (two tabs on one folder; one folder's
 * parent-watch is another's self-watch). The kernel gives them the same wd and would
 * *replace* the mask, so watches are added with IN_MASK_ADD and removed only when the last
 * subscriber of that wd goes away. Subscribers must filter by wd and mask themselves. */
#pragma once

#include "inotify-parse.h"

typedef struct GbkInotifySub GbkInotifySub;

[[nodiscard]] GbkInotifySub *gbk_inotify_sub_new (void);
/* Removes every watch this subscriber added (if no one else uses it) and frees it. After
 * this returns the hub never touches the subscriber again. */
void gbk_inotify_sub_free (GbkInotifySub *sub);

/* Readable when events are buffered. */
int gbk_inotify_sub_fd (const GbkInotifySub *sub);

/* Watch `path`. Returns the wd, or -1 with errno set. */
int gbk_inotify_sub_watch (GbkInotifySub *sub, const char *path, uint32_t mask);

/* Hand every buffered event to fn (same signature as the parser's callback) and reset the
 * fd. If the buffer overflowed, fn first gets one event with mask IN_Q_OVERFLOW, wd -1. */
void gbk_inotify_sub_take (GbkInotifySub *sub, GbkInotifyFn fn, void *data);

/* Test hook: per-subscriber buffer cap. */
void gbk_inotify_set_max_pending_for_test (unsigned n);
#define GBK_INOTIFY_MAX_PENDING 16384
