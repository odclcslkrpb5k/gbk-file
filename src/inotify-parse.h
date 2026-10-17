/* Parse a buffer read from an inotify fd. Pure function: no I/O, so it can be fuzzed. */
#pragma once

/* `name` is nullptr for events about the watched directory itself. Otherwise it is
 * NUL-terminated and `name_len` bytes long. */
typedef void (*GbkInotifyFn) (int wd, uint32_t mask, const char *name, size_t name_len, void *data);

/* Calls fn for each complete event. Returns false if the buffer is malformed (an event
 * header or name runs past the end); events before the bad one have been delivered. */
bool gbk_inotify_parse (const uint8_t *buf, size_t len, GbkInotifyFn fn, void *data);
