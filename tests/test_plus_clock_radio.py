"""Exercise real HalClock lifecycle against a deterministic SNTP/radio seam."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
headers={
'freertos/FreeRTOS.h':'#pragma once\nconstexpr int pdTRUE=1;\nconstexpr unsigned portMAX_DELAY=~0u;\n',
'freertos/semphr.h':'''#pragma once
#include <mutex>
struct StaticSemaphore_t {std::mutex mutex;};
using SemaphoreHandle_t=StaticSemaphore_t*;
inline auto xSemaphoreCreateMutexStatic(StaticSemaphore_t* s){return s;}
inline int xSemaphoreTake(SemaphoreHandle_t s,unsigned){s->mutex.lock();return 1;}
inline void xSemaphoreGive(SemaphoreHandle_t s){s->mutex.unlock();}
''',
'PlusHttpsClock.h':'#pragma once\n#include <ctime>\nextern unsigned httpsCalls; inline bool metalioPlusFetchHttpsUtc(time_t&){++httpsCalls;return false;}\n',
'BoardConfig.h':'#pragma once\n#define FREEINK_DEVICE_METALIO_PLUS 1\n',
'Arduino.h':'''#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstddef>
extern unsigned long clockMillis;
inline unsigned long millis(){return clockMillis;}
#define CONFIG_LWIP_SNTP_UPDATE_DELAY 3600000
#define pdMS_TO_TICKS(x) (x)
''',
'Rtc.h':'''#pragma once
#include <cstdint>
struct Rtc {
 struct DateTime {uint16_t year; uint8_t month,day,hour,minute,second,weekday;};
 bool begin(){return false;}
 bool now(DateTime&){return false;}
 bool set(const DateTime&){return false;}
};
''',
'Logging.h':'#pragma once\n#define LOG_INF(...) ((void)0)\n#define LOG_ERR(...) ((void)0)\n#define LOG_DBG(...) ((void)0)\n',
'WiFi.h':'''#pragma once
constexpr int WL_CONNECTED=3;
struct MockWiFi { int state=WL_CONNECTED; int status(){return state;} };
extern MockWiFi WiFi;
''',
'TrustedTime.h':'#pragma once\nnamespace trustedtime {void note();}\n',
'esp_wifi.h':'''#pragma once
#include "esp_netif.h"
enum wifi_ps_type_t {WIFI_PS_NONE=0,WIFI_PS_MIN_MODEM=1};
esp_err_t esp_wifi_get_ps(wifi_ps_type_t*);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t);
esp_err_t esp_wifi_get_max_tx_power(int8_t*);
esp_err_t esp_wifi_set_max_tx_power(int8_t);
''',
'esp_netif.h':'''#pragma once
using esp_err_t=int;
constexpr int ESP_OK=0;
inline int esp_netif_tcpip_exec(int (*fn)(void*),void* value){return fn(value);}
''',
'esp_sntp.h':'#pragma once\n',
'esp_netif_sntp.h':'''#pragma once
#include "esp_netif.h"
#include <sys/time.h>
struct esp_sntp_config_t {const char* servers[3];bool start=true;bool smooth_sync=false;void (*sync_cb)(timeval*)=nullptr;};
#define ESP_SNTP_SERVER_LIST(...) { __VA_ARGS__ }
#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(n,servers) {servers}
int esp_netif_sntp_init(const esp_sntp_config_t*);
int esp_netif_sntp_start();
void esp_netif_sntp_deinit();
int esp_netif_sntp_sync_wait(unsigned long);
''',
'lwip/apps/sntp.h':'''#pragma once
#include <cstdint>
struct ip_addr_t {int unused;};
inline bool sntp_enabled(){return false;}
inline void sntp_stop(){}
inline const ip_addr_t* sntp_getserver(uint8_t){return nullptr;}
inline void ipaddr_ntoa_r(const ip_addr_t*,char*,unsigned long){}
inline const char* sntp_getservername(uint8_t){return "test";}
inline int sntp_getreachability(uint8_t){return 0;}
'''}
main='''#include "HalClock.h"
#include "WiFi.h"
#include "esp_wifi.h"
#include "esp_netif_sntp.h"
#include <cassert>
unsigned long clockMillis=0;
MockWiFi WiFi;
wifi_ps_type_t ps=WIFI_PS_MIN_MODEM;
int8_t txPower=80;bool powerReadError=false,powerSetError=false,powerReadbackError=false;
bool reply=false, startError=false, live=false;
unsigned notes=0, httpsCalls=0;
namespace trustedtime {void note(){++notes;}}
int esp_wifi_get_ps(wifi_ps_type_t* out){*out=ps;return 0;}
int esp_wifi_set_ps(wifi_ps_type_t mode){ps=mode;return 0;}
int esp_wifi_get_max_tx_power(int8_t*out){if(powerReadError||(powerReadbackError&&txPower==20))return -1;*out=txPower;return 0;}
int esp_wifi_set_max_tx_power(int8_t value){if(powerSetError&&value==20)return -1;txPower=value;return 0;}
int esp_netif_sntp_init(const esp_sntp_config_t*){live=true;return 0;}
int esp_netif_sntp_start(){return startError ? -1 : 0;}
void esp_netif_sntp_deinit(){live=false;}
int esp_netif_sntp_sync_wait(unsigned long){bool value=reply;reply=false;return value ? 0 : -1;}
int main(){
 halClock.setAutoSyncEnabled(false);
 assert(halClock.requestSync() && ps==WIFI_PS_NONE && live);
 assert(txPower==(RICKYOS_PLUS_TX_POWER_COMPAT?20:80));
 halClock.cancelSync();
 assert(ps==WIFI_PS_MIN_MODEM && txPower==80 && !live && halClock.syncState()==ClockSyncState::Idle);
 assert(halClock.requestSync());reply=true;halClock.update();
 assert(halClock.syncState()==ClockSyncState::Succeeded && ps==WIFI_PS_MIN_MODEM && !live && notes==1);
 assert(halClock.requestSync());clockMillis=60001;halClock.update();
 assert(halClock.syncState()==ClockSyncState::Failed && ps==WIFI_PS_MIN_MODEM && !live);
 startError=true;assert(!halClock.requestSync());
 assert(ps==WIFI_PS_MIN_MODEM && !live);startError=false;
 assert(!halClock.syncNow(10));assert(ps==WIFI_PS_MIN_MODEM && !live);
 assert(halClock.requestSync());WiFi.state=0;halClock.update();
 assert(ps==WIFI_PS_MIN_MODEM && !live);
 // NTP starts first, web service outlives it: ending NTP must not sleep WiFi.
 WiFi.state=WL_CONNECTED;
 assert(halClock.requestSync());
 HalWifiAwakeLease web, download;
 assert(web.acquire() && web.acquire()); // idempotent acquire
 assert(HalWifiAwakeLease::activeCount()==2);
 assert(txPower==(RICKYOS_PLUS_TX_POWER_COMPAT?20:80));
 halClock.cancelSync();
 assert(ps==WIFI_PS_NONE && HalWifiAwakeLease::activeCount()==1);
 assert(download.acquire());web.release();web.release();
 assert(ps==WIFI_PS_NONE && HalWifiAwakeLease::activeCount()==1);
 download.release();assert(ps==WIFI_PS_MIN_MODEM && txPower==80 && HalWifiAwakeLease::activeCount()==0);
 // Do not run synchronous HTTPS backup while a web/transfer owner is active.
 assert(halClock.requestSync());assert(web.acquire());
 const unsigned oldHttpsCalls=httpsCalls;
 clockMillis+=21000;halClock.update();assert(httpsCalls==oldHttpsCalls);
 web.release();halClock.update();assert(httpsCalls==oldHttpsCalls+1);
 halClock.cancelSync();assert(ps==WIFI_PS_MIN_MODEM);
 // Same overlapping lifecycle on NTP success and deadline.
 assert(halClock.requestSync());assert(web.acquire());reply=true;halClock.update();
 assert(ps==WIFI_PS_NONE);web.release();assert(ps==WIFI_PS_MIN_MODEM);
 assert(halClock.requestSync());assert(web.acquire());clockMillis+=60001;halClock.update();
 assert(ps==WIFI_PS_NONE);web.release();assert(ps==WIFI_PS_MIN_MODEM);
 // Preserve callers that deliberately keep the radio awake.
 WiFi.state=WL_CONNECTED;ps=WIFI_PS_NONE;
 assert(halClock.requestSync());halClock.cancelSync();assert(ps==WIFI_PS_NONE);
#if RICKYOS_PLUS_TX_POWER_COMPAT
 ps=WIFI_PS_MIN_MODEM;txPower=80;
 HalWifiAwakeLease failed;
 powerReadError=true;assert(!failed.acquire());powerReadError=false;
 assert(ps==WIFI_PS_MIN_MODEM&&txPower==80&&HalWifiAwakeLease::activeCount()==0);
 powerSetError=true;assert(!failed.acquire());powerSetError=false;
 assert(ps==WIFI_PS_MIN_MODEM&&txPower==80&&HalWifiAwakeLease::activeCount()==0);
 powerReadbackError=true;assert(!failed.acquire());powerReadbackError=false;
 assert(ps==WIFI_PS_MIN_MODEM&&txPower==80&&HalWifiAwakeLease::activeCount()==0);
 // Never raise an existing lower power limit.
 txPower=8;assert(failed.acquire()&&txPower==8);failed.release();assert(txPower==8);
 // A separate caller's changed value must not be overwritten on final release.
 txPower=80;assert(failed.acquire()&&txPower==20);txPower=44;failed.release();assert(txPower==44);
 assert(ps==WIFI_PS_MIN_MODEM&&HalWifiAwakeLease::activeCount()==0);
#endif

}
'''
with tempfile.TemporaryDirectory() as directory:
 work=Path(directory)
 for name,content in headers.items():
  path=work/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(content)
 (work/'main.cpp').write_text(main)
 source=root/'firmware/rickyos-plus-1.2/upstream/lib/hal'
 binary=work/'test'
 for compat in [0,1]:
  subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Wno-unused-variable','-DRICKYOS_PLUS_TX_POWER_COMPAT='+str(compat),'-I'+str(work),'-I'+str(source),'-I'+str(root/'firmware/rickyos-plus-1.2/components/rickyos_application/port'),str(source/'HalClock.cpp'),str(work/'main.cpp'),'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
print('Real HalClock: success, cancellation, deadline, start failure, wait failure disconnect and overlapping web/download owners restore radio mode')
