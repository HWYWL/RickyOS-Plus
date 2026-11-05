"""Execute the production periodic memory block with a disconnected Arduino console.

Plus owns its IDF console independently. A false Arduino Serial must not hide
its internal/DMA/PSRAM telemetry. Other targets retain their transport gate.
"""
from pathlib import Path
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2'
source = (PORT / 'upstream/src/main.cpp').read_text()
start = source.index('  // Plus logs through IDF, not Arduino Serial')
end = source.index('  // Handle incoming serial commands,', start)
block = source[start:end]
program = r'''
#include <cassert>
#include <cstddef>
#include <cstring>
#define BOARD_HAS_PSRAM 1
#define FREEINK_LOG_TRANSPORT_ROM_PRINTF 2
unsigned tick=0, logs=0, internalReads=0, dmaReads=0, psramReads=0;
bool Serial=false;
unsigned millis(){return tick;}
namespace HalMemory {
struct HeapStats {size_t freeBytes=1,totalBytes=2,minFreeBytes=3,largestBlockBytes=4;};
HeapStats getInternalHeap(){++internalReads;return {};}
HeapStats getInternalDmaHeap(){++dmaReads;return {};}
HeapStats getPsramHeap(){++psramReads;return {};}
}
template<class... T> void record(const char* tag,const char*,T...){assert(!strcmp(tag,"MEM"));++logs;}
#define LOG_INF(...) record(__VA_ARGS__)
namespace bleinput {bool isRunning(){return false;}void logDiagnostics(const char*){assert(false);}}
unsigned lastMemPrint=0;
struct RenderLock {static bool held; static bool peek(){return held;}};
bool RenderLock::held=false;
void loop(){
''' + block + r'''
}
int main(){
 tick=9999;loop();assert(logs==0);
 tick=10000;loop();
#if FREEINK_DEVICE_METALIO_PLUS || FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_ROM_PRINTF
 assert(internalReads==1&&psramReads==1);
 assert(dmaReads==FREEINK_DEVICE_METALIO_PLUS);
 assert(logs==2+FREEINK_DEVICE_METALIO_PLUS&&lastMemPrint==10000);
 tick=19999;loop();assert(internalReads==1);
 tick=20000;loop();assert(internalReads==2);
 // A page rendering or driving the panel holds the render lock: the heap walk
 // (interrupts off) waits for the next idle loop rather than stalling the panel feed.
 tick=30000;RenderLock::held=true;loop();assert(internalReads==2&&lastMemPrint==20000);
 RenderLock::held=false;loop();assert(internalReads==3&&lastMemPrint==30000);
#else
 assert(logs==0&&lastMemPrint==0);
 Serial=true;loop();assert(internalReads==1&&psramReads==1&&dmaReads==0);
#endif
}
'''
with tempfile.TemporaryDirectory(prefix='plus-memory-log-') as folder:
    path = Path(folder)
    (path / 'test.cpp').write_text(program)
    for plus, transport in ((1, 0), (0, 0), (0, 2)):
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', f'-DFREEINK_DEVICE_METALIO_PLUS={plus}',
                        f'-DFREEINK_LOG_TRANSPORT={transport}', str(path / 'test.cpp'),
                        '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)
print('Actual memory telemetry: Plus IDF console independent of Arduino, DMA subset, '
      '10 s bound, deferral while rendering and other transport gates passed (ASan/UBSan)')
