"""Run the production Plus TCP policy with deterministic sockets and a fake clock."""
from pathlib import Path
import subprocess
import tempfile

source = Path('firmware/rickyos-plus-1.2/upstream/freeink-sdk/libs/network/SecureNet/src/SecureClient.cpp').read_text()
a = source.index('  const uint32_t dnsElapsed = millis() - started;')
b = source.index('\n#else', a)
policy = source[a:b]
code = r'''#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>
#define LOG_INF(...) ((void)0)
uint32_t clockMs = 0;
uint32_t millis() { return clockMs; }
struct Socket {
  uint32_t timeout = 0;
  std::vector<uint32_t> durations, observed;
  std::vector<bool> results;
  unsigned calls = 0;
  void setConnectionTimeout(uint32_t ms) { timeout = ms; }
  bool connect(int, uint16_t) {
    observed.push_back(timeout);
    assert(calls < durations.size() && calls < results.size());
    assert(durations[calls] <= timeout);
    clockMs += durations[calls];
    return results[calls++];
  }
};
struct Client {
  Socket _transport;
  bool aborted = false;
  bool abortRequested() { return aborted; }
  int run(uint32_t started, uint32_t timeoutMs) {
    int resolved = 0;
    uint16_t port = 443;
    _transport.setConnectionTimeout(timeoutMs);
''' + policy + r'''
    return tcpConnected;
  }
};
int main() {
  // First socket succeeds: restore the caller's I/O timeout too.
  clockMs=1300; Client first; first._transport.durations={300}; first._transport.results={true};
  assert(first.run(0,12000)==1 && first._transport.timeout==12000);
  assert(first._transport.observed==std::vector<uint32_t>({4000}));
  // Long DNS + first timeout leaves only one second for the second socket.
  clockMs=7000; Client retry; retry._transport.durations={4000,900}; retry._transport.results={false,true};
  assert(retry.run(0,12000)==1 && retry._transport.timeout==12000);
  assert(retry._transport.observed==std::vector<uint32_t>({4000,1000}));
  // Plus retry gets enough time to exercise a delayed path, inside one budget.
  clockMs=500; Client delayed; delayed._transport.durations={4000,12000}; delayed._transport.results={false,true};
  assert(delayed.run(0,30000)==1 && delayed._transport.timeout==30000);
  assert(delayed._transport.observed==std::vector<uint32_t>({4000,15000}));
  // Exhausting the budget suppresses another socket; failure still restores timeout.
  clockMs=9000; Client exhausted; exhausted._transport.durations={3000}; exhausted._transport.results={false};
  assert(exhausted.run(0,12000)==0 && exhausted._transport.calls==1 && exhausted._transport.timeout==12000);
  clockMs=12000; Client dns; assert(dns.run(0,12000)==0 && dns._transport.calls==0);
  clockMs=0; Client cancelled; cancelled.aborted=true;
  assert(cancelled.run(0,12000)==0 && cancelled._transport.calls==0);
  // Unsigned elapsed arithmetic remains valid across millis() rollover.
  clockMs=100; Client wrap; wrap._transport.durations={1}; wrap._transport.results={true};
  assert(wrap.run(UINT32_MAX-100,12000)==1 && wrap._transport.timeout==12000);
}
'''
with tempfile.TemporaryDirectory() as folder:
    path=Path(folder); (path/'test.cpp').write_text(code)
    subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Production Plus TCP policy: first success, retry budget, exhaustion, cancellation and clock rollover passed')
