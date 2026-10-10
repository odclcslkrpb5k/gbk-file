/* gbk_fatal(): log and abort. Use for violated invariants that must never be survived.
 *
 * Replaces g_error(), which is banned: its trailing `for (;;);` trips GCC -fanalyzer
 * (analyzer-infinite-loop), and suppressing the analyzer would hide real findings. */
#pragma once

[[noreturn]] G_GNUC_PRINTF (1, 2) static inline void
gbk_fatal (const char *format, ...)
{
  va_list args;
  va_start (args, format);
  g_logv (G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, format, args);
  va_end (args);
  abort ();
}
