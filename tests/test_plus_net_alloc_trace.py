"""Fault-inject the actual optional wrappers; not a radio/throughput test."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'firmware/rickyos-plus-1.2/components/rickyos_application/port'
HEADERS = {
    'esp_attr.h': '#pragma once\n#define IRAM_ATTR\n',
    'esp_heap_caps.h': '''#pragma once
#include <cstddef>
constexpr int MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2, MALLOC_CAP_DMA=4;
inline size_t heap_caps_get_free_size(int){return 24000;}
inline size_t heap_caps_get_largest_free_block(int){return 8000;}
''',
    'esp_log.h': '''#pragma once
void recordLog(const char*, const char*, ...);
#define ESP_LOGI recordLog
''',
    'esp_netif.h': '''#pragma once
#include <cstddef>
using esp_err_t=int;
constexpr int ESP_ERR_NO_MEM=0x101;
constexpr int ESP_ERR_INVALID_ARG=0x102;
struct esp_netif_t {int id;};
''',
    'esp_netif_net_stack.h': '#pragma once\n#include "esp_netif.h"\n',
    'esp_private/wifi.h': '''#pragma once
#include <cstdint>
#include "esp_netif_net_stack.h"
using wifi_interface_t=int;
''',
    'freertos/FreeRTOS.h': '#pragma once\n',
    'freertos/portmacro.h': '''#pragma once
#include <mutex>
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
#define portENTER_CRITICAL_SAFE(p) (p)->lock()
#define portEXIT_CRITICAL_SAFE(p) (p)->unlock()
''',
    'lwip/pbuf.h': '''#pragma once
#include <cstdint>
using u16_t=uint16_t;
enum pbuf_layer {PBUF_TRANSPORT};
enum pbuf_type {PBUF_RAM};
struct pbuf {int marker;};
''',
}
MAIN = r'''
#include "PlusNetAllocTrace.cpp"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
std::vector<std::string> lines;
void recordLog(const char*,const char* format,...){
 char text[512];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);
 lines.emplace_back(text);
}
pbuf packet{7}; bool packetFail=false; int driverResult=0;
unsigned copyCalls=0, referenceCalls=0;
char data[256]; esp_netif_t netif{9};
extern "C" {
void* __real_heap_caps_malloc(size_t,uint32_t){errno=75;return nullptr;}
void* __real_heap_caps_calloc(size_t,size_t,uint32_t){errno=76;return nullptr;}
void* __real_heap_caps_realloc(void*,size_t,uint32_t){errno=77;return nullptr;}
pbuf* __real_pbuf_alloc(pbuf_layer l,u16_t n,pbuf_type t){
 assert(l==PBUF_TRANSPORT && t==PBUF_RAM && n>0);errno=71;return packetFail?nullptr:&packet;
}
int __real_esp_netif_transmit_wrap(esp_netif_t* p,void* d,size_t n,void* r){
 assert(p==&netif&&d==data&&n==100&&r==&packet);errno=72;return driverResult;
}
int __real_esp_wifi_internal_tx(wifi_interface_t i,void* d,uint16_t n){
 assert(i==0&&d==data&&n==100);++copyCalls;errno=73;return driverResult;
}
int __real_esp_wifi_internal_tx_by_ref(wifi_interface_t i,void* d,size_t n,void* r){
 assert(i==0&&d==data&&n==100&&r==&packet);++referenceCalls;errno=74;return driverResult;
}
}
bool has(const std::string& needle){for(const auto& line:lines)if(line.find(needle)!=std::string::npos)return true;return false;}
int main(){
 assert(__wrap_pbuf_alloc(PBUF_TRANSPORT,64,PBUF_RAM)==&packet&&errno==71);
 packetFail=true;
 assert(!__wrap_pbuf_alloc(PBUF_TRANSPORT,128,PBUF_RAM)&&errno==71);
 assert(!__wrap_pbuf_alloc(PBUF_TRANSPORT,64,PBUF_RAM));
 packetFail=false;
 for(int result:{0,ESP_ERR_NO_MEM,-77}){
  driverResult=result;
  assert(__wrap_esp_netif_transmit_wrap(&netif,data,100,&packet)==result&&errno==72);
  assert(__wrap_esp_wifi_internal_tx(0,data,100)==result&&errno==73);
  assert(__wrap_esp_wifi_internal_tx_by_ref(0,data,100,&packet)==result&&errno==74);
 }
 assert(lines.empty()); // Hooks themselves never print.
 plusNetAllocTraceSnapshot("test");
 assert(has("layer=pbuf calls=3 failed=2 nomem=2 max_failed_bytes=128 last=-1"));
 for(const char* layer:{"netif","wifi-copy","wifi-ref"})
  assert(has(std::string("layer=")+layer+" calls=3 failed=2 nomem=1 max_failed_bytes=100 last=-77"));
 assert(has("internal_free=24000 internal_largest=8000 dma_free=24000 dma_largest=8000"));
 // Concurrent receive/send callers must not lose counter updates.
 std::thread a([]{for(int i=0;i<2000;++i)__wrap_pbuf_alloc(PBUF_TRANSPORT,32,PBUF_RAM);});
 std::thread b([]{for(int i=0;i<2000;++i)__wrap_pbuf_alloc(PBUF_TRANSPORT,32,PBUF_RAM);});
 a.join();b.join();lines.clear();plusNetAllocTraceSnapshot("after");
 assert(has("layer=pbuf calls=4003 failed=2 nomem=2 max_failed_bytes=128 last=-1"));
 // Failure-only ring is finite; realloc(ptr,0) and zero-size allocation are
 // not failures. Allocator return values/errno must remain untouched.
 assert(!__wrap_heap_caps_realloc(&packet,0,3)&&errno==77);
 for(unsigned i=1;i<=12;++i)assert(!__wrap_heap_caps_malloc(i,5)&&errno==75);
 assert(!__wrap_heap_caps_calloc(2,10,7)&&errno==76);
 lines.clear();plusNetAllocTraceSnapshot("heap");
 assert(has("allocation_failures=13"));
 assert(has("allocation_serial=13 kind=1 bytes=20 caps=0x7"));
 assert(!has("allocation_serial=1 "));
#if RICKYOS_PLUS_NET_COPY_TX
 // Switch before an independent round. Never attempt reference-send then
 // retry-copy, and never touch the caller's pbuf reference counter.
 const unsigned oldCopy=copyCalls, oldRef=referenceCalls;
 plusNetAllocTraceSetCopyTx(true);
 driverResult=ESP_ERR_NO_MEM;
 assert(__wrap_esp_wifi_internal_tx_by_ref(0,data,100,&packet)==ESP_ERR_NO_MEM&&errno==73);
 assert(copyCalls==oldCopy+1&&referenceCalls==oldRef);
 assert(__wrap_esp_wifi_internal_tx_by_ref(0,data,size_t(UINT16_MAX)+1,&packet)==ESP_ERR_INVALID_ARG);
 assert(copyCalls==oldCopy+1&&referenceCalls==oldRef);
 plusNetAllocTraceSetCopyTx(false);
 driverResult=0;
 assert(__wrap_esp_wifi_internal_tx_by_ref(0,data,100,&packet)==0&&errno==74);
 assert(copyCalls==oldCopy+1&&referenceCalls==oldRef+1);
#endif
}
'''

with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    for name, text in HEADERS.items():
        target = work / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    (work / 'test.cpp').write_text(MAIN)
    for copy_mode in (0, 1):
        binary = work / f'test-{copy_mode}'
        subprocess.run(['c++', '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-DRICKYOS_PLUS_NET_ALLOC_TRACE=1',
                        f'-DRICKYOS_PLUS_NET_COPY_TX={copy_mode}', '-I', str(work), '-I', str(PORT),
                        str(work/'test.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    # Without the opt-in macro, there must be no SDK references or hooks.
    subprocess.run(['c++', '-c', str(PORT/'PlusNetAllocTrace.cpp'), '-o', str(work/'off.o')], check=True)
    symbols = subprocess.check_output(['nm', str(work/'off.o')], text=True)
    assert 'wrap_' not in symbols and 'plusNetAllocTrace' not in symbols
print('Plus allocation trace: transparent returns/errno, failures, concurrency, copy/reference switching and default-off passed')
