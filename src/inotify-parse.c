#include "inotify-parse.h"

bool
gbk_inotify_parse (const uint8_t *buf, size_t len, GbkInotifyFn fn, void *data)
{
  size_t off = 0;
  while (off < len)
    {
      struct inotify_event ev;
      size_t name_start, end;
      if (ckd_add (&name_start, off, sizeof ev) || name_start > len)
        return false;
      memcpy (&ev, buf + off, sizeof ev);       /* buffer may be unaligned */
      if (ckd_add (&end, name_start, (size_t) ev.len) || end > len)
        return false;

      if (ev.len == 0)
        fn (ev.wd, ev.mask, nullptr, 0, data);
      else
        {
          /* The kernel NUL-pads the name to ev.len; don't trust that a NUL is present. */
          const char *raw = (const char *) buf + name_start;
          size_t name_len = strnlen (raw, ev.len);
          if (name_len == ev.len)
            return false;
          if (name_len == 0)
            fn (ev.wd, ev.mask, nullptr, 0, data);
          else
            fn (ev.wd, ev.mask, raw, name_len, data);
        }
      off = end;
    }
  return true;
}
