"""Exercise the real loan-clock code with forbidden legacy SNTP APIs."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
# Confirm the stub selector matches the actual application compiler flags.
import json, shlex
commands = json.loads((root / '.local/build-rickyos-plus-1.2/compile_commands.json').read_text())
assert any('-DFREEINK_DEVICE_METALIO_PLUS=1' in shlex.split(item['command'])
           for item in commands if item['file'].endswith('/TrustedTime.cpp'))
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    headers = {
        'BoardConfig.h': '#pragma once\n#define FREEINK_DEVICE_METALIO_PLUS 1\n',
        'Logging.h': '#pragma once\n#define LOG_DBG(...) ((void)0)\n',
        'Arduino.h': '''#pragma once
#include <cstdint>
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
inline unsigned long millis() { return 0; }
inline void delay(int) {}
void configTzTime(const char*, const char*);
''',
        'Preferences.h': '''#pragma once
#include <cstdint>
struct Preferences {
 bool begin(const char*, bool) { return true; }
 int64_t getLong64(const char*, int64_t fallback) { return fallback; }
 void putLong64(const char*, int64_t) {}
 void end() {}
};
''',
        'esp_sntp.h': '''#pragma once
#include <sys/time.h>
constexpr int SNTP_SYNC_STATUS_COMPLETED = 1;
void sntp_set_time_sync_notification_cb(void (*)(timeval*));
bool esp_sntp_enabled();
int sntp_get_sync_status();
''',
        'HalClock.h': '''#pragma once
#include <cstdint>
struct HalClock {
 unsigned starts = 0, waits = 0;
 uint32_t timeout = 0;
 bool result = false;
 bool requestSync() { ++starts; return true; }
 bool syncNow(uint32_t value) { ++waits; timeout = value; return result; }
};
extern HalClock halClock;
''',
    }
    for name, content in headers.items():
        (work / name).write_text(content)
    (work / 'main.cpp').write_text('''#include <cassert>
#include <cstdlib>
#include "HalClock.h"
#include "TrustedTime.h"
#include <sys/time.h>
HalClock halClock;
void configTzTime(const char*, const char*) { std::abort(); }
void sntp_set_time_sync_notification_cb(void (*)(timeval*)) { std::abort(); }
bool esp_sntp_enabled() { std::abort(); }
int sntp_get_sync_status() { std::abort(); }
int main() {
 trustedtime::init(); // Must not replace the owner's completion callback.
 trustedtime::startSync();
 assert(halClock.starts == 1);
 assert(!trustedtime::syncNow(5000));
 assert(halClock.waits == 1 && halClock.timeout == 5000);
 halClock.result = true;
 assert(trustedtime::syncNow(1234));
 assert(halClock.waits == 2 && halClock.timeout == 1234);
}
''')
    source = root / 'firmware/rickyos-plus-1.2/upstream/lib/TrustedTime'
    binary = work / 'test'
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-I'+str(work), '-I'+str(source),
                    str(source / 'TrustedTime.cpp'), str(work / 'main.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Plus loan-clock delegation, callback ownership and deadline/result propagation passed')
