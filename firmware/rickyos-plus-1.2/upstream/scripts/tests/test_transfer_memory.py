"""Compile real transfer memory policy against bounded HAL/failure seams."""
from pathlib import Path
import tempfile
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class TransferMemoryTest(unittest.TestCase):
    def check(self, program, defines=()):
        with tempfile.TemporaryDirectory(prefix='transfer-memory-') as directory:
            stubs = Path(directory)
            (stubs / 'BoardConfig.h').write_text('''#pragma once
#if defined(SIMULATOR_DEVICE_READPICO) && !defined(FREEINK_DEVICE_READPICO)
#define FREEINK_DEVICE_READPICO 1
#endif
''')
            (stubs / 'Logging.h').write_text('''#pragma once
template<class... T> void ignoreLog(T&&...) {}
#define LOG_INF(...) ignoreLog(__VA_ARGS__)
#define LOG_ERR(...) ignoreLog(__VA_ARGS__)
''')
            (stubs / 'HalMemory.h').write_text('''#pragma once
#include <cstdlib>
#include <memory>
struct HalMemory {
 struct HeapStats {size_t freeBytes,totalBytes,minFreeBytes,largestBlockBytes;};
 static inline HeapStats internal{65536,100000,65536,32768};
 static inline HeapStats psram{6000000,8000000,6000000,6000000};
 static inline bool failPsram=false;
 static inline size_t requests=0, requestedBytes=0;
 struct PsramDeleter {void operator()(uint8_t* p) const {std::free(p);}};
 using PsramBuffer=std::unique_ptr<uint8_t[],PsramDeleter>;
 static HeapStats getInternalHeap() {return internal;}
 static HeapStats getPsramHeap() {return psram;}
 static PsramBuffer allocatePsram(size_t n) {
  ++requests;requestedBytes+=n;
  return PsramBuffer{failPsram?nullptr:static_cast<uint8_t*>(std::calloc(1,n))};
 }
};
''')
            run_cpp('#include <cstdint>\n#include <cassert>\n#include "TransferMemory.h"\n' + program,
                    include_dirs=(stubs, ROOT / 'src/network', ROOT / 'lib/Memory'),
                    defines=('CROSSPOINT_EMULATED', *defines))

    def test_psram_buffers_and_failure_never_consume_internal_fallback(self):
        self.check(r'''
int main() {
 assert(transferMemory::guarded);
 auto a=transferMemory::allocateBuffer(4096);
 auto b=transferMemory::allocateBuffer(4096);
 auto c=transferMemory::allocateBuffer(1400);
 assert(a && b && c && HalMemory::requestedBytes==9592 && HalMemory::requests==3);
 HalMemory::failPsram=true;
 assert(!transferMemory::allocateBuffer(4096));
 assert(!transferMemory::allocateBuffer(0));
 HalMemory::psram={0,0,0,0};
 HalMemory::internal.freeBytes=transferMemory::START_FREE+4095;
 assert(!transferMemory::allocateBuffer(4096));
 HalMemory::internal.freeBytes=transferMemory::START_FREE+4096;
 HalMemory::internal.largestBlockBytes=4095;
 assert(!transferMemory::allocateBuffer(4096));
 HalMemory::internal.largestBlockBytes=4096;
 assert(transferMemory::allocateBuffer(4096));
}
''', defines=('RICKYOS_PRODUCT=1', 'FREEINK_DEVICE_READPICO=1'))

    def test_other_targets_and_simulator_keep_normal_allocation(self):
        for defines in (('RICKYOS_PRODUCT=1', 'FREEINK_DEVICE_READPICO=1', 'SIMULATOR'),):
            self.check('''int main() {
 auto a=transferMemory::allocateBuffer(4096);
 assert(a && HalMemory::requests==0);
 assert(!transferMemory::allocateBuffer(0));
}''', defines=defines)

    def test_startup_and_runtime_thresholds(self):
        self.check('''int main() {
 for(bool startup : {false,true}) {
  size_t free=startup?transferMemory::START_FREE:transferMemory::RUN_FREE;
  size_t block=startup?transferMemory::START_BLOCK:transferMemory::RUN_BLOCK;
  assert(transferMemory::healthy({free,100000,0,block},startup));
  assert(!transferMemory::healthy({free-1,100000,0,block},startup));
  assert(!transferMemory::healthy({free,100000,0,block-1},startup));
 }
}''')

    def test_header_resolves_simulator_device_before_policy(self):
        self.check('''int main() {
 static_assert(transferMemory::guarded);
 assert(!transferMemory::healthy({4096,100000,0,36},true));
}''', defines=('RICKYOS_PRODUCT=1', 'SIMULATOR', 'SIMULATOR_DEVICE_READPICO'))

    def test_actual_service_check_and_periodic_stop(self):
        source = (ROOT / 'src/network/CrossPointWebServer.cpp').read_text()
        check = method(source, 'bool CrossPointWebServer::checkMemoryReserve(')
        client = method(source, 'void CrossPointWebServer::handleClient(')
        gate = method(client, 'if constexpr (transferMemory::guarded)')
        self.check(r'''
unsigned long now=0;
unsigned long millis() {return now;}
struct CrossPointWebServer {
 bool memoryError=false;
 unsigned long lastMemoryCheck=0;
 int stopped=0,requests=0;
 bool checkMemoryReserve(bool);
 void stop() {++stopped;}
 void handle() {
''' + gate + r'''
 ++requests;
 }
};
''' + check + r'''
int main() {
 CrossPointWebServer server;
 assert(server.checkMemoryReserve(true));
 HalMemory::internal={2135,253227,2032,36};
 assert(!server.checkMemoryReserve(true) && server.memoryError);
 now=250;server.handle();assert(server.stopped==1 && server.requests==0);
 HalMemory::internal={32768,100000,2032,8192};
 now=500;server.handle();assert(server.stopped==1 && server.requests==1);
}
''', defines=('RICKYOS_PRODUCT=1', 'FREEINK_DEVICE_READPICO=1'))

    def test_error_releases_services_without_erasing_credentials_or_restarting(self):
        source = (ROOT / 'src/activities/network/CrossPointWebServerActivity.cpp').read_text()
        body = method(source, 'void CrossPointWebServerActivity::showMemoryError(')
        self.check(r'''
enum class WebServerActivityState {SERVER_RUNNING,MEMORY_ERROR};
int stops=0,dnsStops=0,mdnsStops=0,radioStops=0,updates=0;
void stopDnsServer() {++dnsStops;}
struct Mdns {void end() {++mdnsStops;}} MDNS;
struct Wifi {
 void softAPdisconnect(bool off) {assert(off);++radioStops;}
 void disconnect(bool off,bool erase=false) {assert(off && !erase);++radioStops;}
} WiFi;
struct Server {void stop() {++stops;}};
struct CrossPointWebServerActivity {
 WebServerActivityState state=WebServerActivityState::SERVER_RUNNING;
 std::unique_ptr<Server> webServer=std::make_unique<Server>();
 bool isApMode=false;
 void requestUpdate() {++updates;}
 void showMemoryError();
};
''' + body + r'''
int main() {
 for(bool ap : {false,true}) {
  CrossPointWebServerActivity activity;activity.isApMode=ap;activity.showMemoryError();
  assert(activity.state==WebServerActivityState::MEMORY_ERROR);
 }
 assert(stops==2 && dnsStops==2 && mdnsStops==2 && radioStops==3 && updates==2);
}
''')
        self.assertNotIn('restart', body)
        self.assertNotIn('onGoHome(', body)


if __name__ == '__main__':
    unittest.main()
