#pragma once

// Explicit diagnostic build only. Normal firmware has no counters or wrappers.
#if RICKYOS_PLUS_NET_ALLOC_TRACE
void plusNetAllocTraceSnapshot(const char* stage);
#if RICKYOS_PLUS_NET_COPY_TX
void plusNetAllocTraceSetCopyTx(bool enabled);
#endif
#endif
