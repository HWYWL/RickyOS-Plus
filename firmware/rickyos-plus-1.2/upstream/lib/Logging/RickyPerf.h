#pragma once

// Timing lines for perf builds (envs rickyos_readpico_perf / rickyos_eego_a4_perf, or
// Plus with RICKYOS_PERF=1). Each measured event prints one line:
//
//   [12345] [INF] [PERF] <event> [<label>] key=<ms> key=<ms> ...
//
// tools/rickyos-perf/perf.py captures the serial log and tabulates median / p90 / max
// per event and key. Values are milliseconds unless the key names another unit
// (kb, kbps, n). Release builds compile none of this.

#ifndef RICKYOS_PERF
#define RICKYOS_PERF 0
#endif

#if RICKYOS_PERF
#include <Logging.h>
#define RICKY_PERF(event, format, ...) logPrintf("INF", "PERF", event " " format "\n", ##__VA_ARGS__)
#else
#define RICKY_PERF(event, format, ...) ((void)0)
#endif
