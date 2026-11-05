#pragma once

// Standby wake diagnostics, compiled in only with -DRICKYOS_STANDBY_DIAG=1. Light sleep
// drops the USB serial link, so lines written around a wake are lost: each note also
// goes into a ring that is printed again once the device has stayed awake.
#ifdef RICKYOS_STANDBY_DIAG
#include <Arduino.h>
#include <Logging.h>

#include <cstdarg>
#include <cstdio>

namespace standby_diag {
constexpr int kLines = 48;
constexpr int kLen = 112;
inline char g_lines[kLines][kLen];
inline int g_next = 0;
inline int g_count = 0;
inline unsigned long g_lastNoteMs = 0;
inline unsigned long g_lastDumpMs = 0;

inline void note(const char* fmt, ...) {
  char* line = g_lines[g_next];
  int used = snprintf(line, kLen, "[%lu] ", millis());
  if (used < 0 || used >= kLen) used = 0;
  va_list args;
  va_start(args, fmt);
  vsnprintf(line + used, kLen - used, fmt, args);
  va_end(args);
  g_next = (g_next + 1) % kLines;
  if (g_count < kLines) ++g_count;
  g_lastNoteMs = millis();
  LOG_INF("SBDIAG", "%s", line);
}

// Called from the main loop: reprint the ring 3 s after the latest note, and every
// 30 s, so a host that reconnected after a light sleep still sees it.
inline void dumpWhenAwake() {
  const unsigned long now = millis();
  const bool fresh = g_lastNoteMs > g_lastDumpMs && now - g_lastNoteMs >= 3000;
  if (!fresh && now - g_lastDumpMs < 30000) return;
  g_lastDumpMs = now;
  LOG_INF("SBDIAG", "---- %d notes ----", g_count);
  for (int i = 0; i < g_count; ++i) LOG_INF("SBDIAG", "%s", g_lines[(g_next - g_count + i + kLines) % kLines]);
}
}  // namespace standby_diag
#define STANDBY_DIAG(...) standby_diag::note(__VA_ARGS__)
#else
#define STANDBY_DIAG(...) ((void)0)
#endif
