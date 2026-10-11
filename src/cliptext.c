#include "cliptext.h"

#define MAX_CLIP_BYTES (16u << 20)

static char *
join_uris (GPtrArray *paths, const char *sep, const char *prefix, bool trailing)
{
  GString *s = g_string_new (prefix);
  for (guint i = 0; i < paths->len; i++)
    {
      g_autofree char *uri = g_filename_to_uri (g_ptr_array_index (paths, i), nullptr, nullptr);
      if (uri == nullptr)
        continue;                     /* not absolute: can't express it */
      if (i > 0 || prefix[0] != '\0')
        if (s->len > 0)
          g_string_append (s, sep);
      g_string_append (s, uri);
    }
  if (trailing && s->len > 0)
    g_string_append (s, sep);
  return g_string_free (s, false);
}

char *
gbk_clip_format_gnome (GPtrArray *paths, bool cut)
{
  return join_uris (paths, "\n", cut ? "cut" : "copy", false);
}

char *
gbk_clip_format_uri_list (GPtrArray *paths)
{
  return join_uris (paths, "\r\n", "", true);
}

char *
gbk_clip_format_text (GPtrArray *paths)
{
  GString *s = g_string_new ("");
  for (guint i = 0; i < paths->len; i++)
    {
      g_autofree char *shown = g_filename_display_name (g_ptr_array_index (paths, i));
      if (i > 0)
        g_string_append_c (s, '\n');
      g_string_append (s, shown);
    }
  return g_string_free (s, false);
}

GPtrArray *
gbk_clip_parse (const char *data, size_t len, bool gnome, bool *cut)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  *cut = false;
  if (data == nullptr || len > MAX_CLIP_BYTES)
    return out;
  g_autofree char *text = g_strndup (data, len);   /* stops at an embedded NUL */
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);

  for (size_t i = 0; lines[i] != nullptr; i++)
    {
      char *line = g_strstrip (lines[i]);           /* also drops the \r of CRLF */
      if (line[0] == '\0' || line[0] == '#')
        continue;
      if (gnome && i == 0)
        {
          *cut = g_str_equal (line, "cut");
          if (g_str_equal (line, "cut") || g_str_equal (line, "copy"))
            continue;
        }
      /* Ask for the hostname: otherwise GLib silently drops it, and file://otherhost/x
       * would become our local /x. Only local URIs are files we can act on. */
      g_autofree char *host = nullptr;
      g_autofree char *path = g_filename_from_uri (line, &host, nullptr);
      if (path == nullptr || !g_path_is_absolute (path))
        continue;
      if (host != nullptr && host[0] != '\0' && !g_str_equal (host, "localhost"))
        continue;
      g_ptr_array_add (out, g_steal_pointer (&path));
    }
  return out;
}
