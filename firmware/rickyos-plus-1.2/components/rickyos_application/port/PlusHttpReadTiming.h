#pragma once

#include <Arduino.h>
#include <Client.h>
#include <Logging.h>

// One body-reader scope; no growth of SecureClient or fixed HTTP workspaces.
// Count nonpositive reads separately from time actually spent inside TLS/TCP.
struct PlusHttpReadTiming {
  uint64_t readUs = 0;
  uint32_t calls = 0;
  uint32_t waits = 0;
  uint32_t bytes = 0;
  uint32_t started = millis();
  int read(Client& client, uint8_t* buffer, size_t size) {
    const uint32_t before = micros();
    const int n = client.read(buffer, size);
    readUs += static_cast<uint32_t>(micros() - before);
    ++calls;
    if (n > 0) bytes += static_cast<uint32_t>(n);
    else ++waits;
    return n;
  }
  ~PlusHttpReadTiming() {
    LOG_INF("HTTP", "Body timing: bytes=%u calls=%u emptyReads=%u readMs=%llu totalMs=%u", bytes, calls, waits,
            static_cast<unsigned long long>(readUs / 1000), static_cast<unsigned>(millis() - started));
  }
};
