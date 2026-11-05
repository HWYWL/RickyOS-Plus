"""Compile the actual radio comparison: bound waits and preserve STA/auto-reconnect."""
from pathlib import Path
import subprocess
import tempfile

source = Path('firmware/rickyos-plus-1.2/components/rickyos_application/port/PlusRadioCompare.cpp')
with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    (root / 'lwip').mkdir()
    (root / 'production.cpp').write_bytes(source.read_bytes())
    (root / 'Arduino.h').write_text('''#pragma once
#include <cstdint>
inline uint32_t now=0;inline uint32_t millis(){return now;}
inline void delay(unsigned ms){now+=ms;}
''')
    (root / 'Logging.h').write_text('''#pragma once
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
''')
    (root / 'WiFi.h').write_text('''#pragma once
#include <cassert>
#include <cstdint>
constexpr int WL_CONNECTED=3;inline uint8_t mask=7;
inline bool radioConnected=true, automatic=true, failBg=false, failFirstBgn=false;
inline unsigned joins=0, bgnJoins=0;
struct FakeWifi {
 int STA=1;bool mode(int){radioConnected=false;return true;}
 int status(){return radioConnected?3:6;}
 bool disconnect(bool off,bool erase,unsigned){assert(!off&&!erase);radioConnected=false;return true;}
 bool reconnect(){++joins;if(mask==7)++bgnJoins;
  radioConnected=mask!=0x47 && !(failFirstBgn&&mask==7&&bgnJoins==1);return true;}
 bool getAutoReconnect(){return automatic;}
 bool setAutoReconnect(bool v){automatic=v;return true;}
 int RSSI(){return -61;}
};inline FakeWifi WiFi;
''')
    (root / 'esp_private').mkdir()
    (root / 'esp_private/wifi.h').write_text('#pragma once\ninline bool rejectFixed=false;inline unsigned fixedCalls=0;constexpr int WIFI_PHY_RATE_6M=11;inline int esp_wifi_internal_set_fix_rate(int,bool,int){++fixedCalls;return rejectFixed?-1:0;}\n')
    (root / 'Network.h').write_text('''#pragma once
#include <cstdint>
inline int dnsResult=1;
struct IPAddress {uint8_t values[4]{36,248,13,186};IPAddress()=default;IPAddress(uint8_t a,uint8_t b,uint8_t c,uint8_t d):values{a,b,c,d}{} uint8_t operator[](unsigned i)const{return values[i];}};
struct FakeNetwork {bool setDefaultInterface(int){return true;}int hostByName(const char*,IPAddress&){return dnsResult;}};inline FakeNetwork Network;
''')
    (root / 'esp_wifi.h').write_text('''#pragma once
#include "WiFi.h"
using esp_err_t=int;constexpr int ESP_OK=0,ESP_FAIL=-1,WIFI_PS_NONE=0;
inline int esp_wifi_set_ps(int){return 0;}
constexpr int WIFI_OFF=0,WIFI_STA=1,WIFI_STORAGE_RAM=0;
inline int8_t txPower=80;inline bool rejectPower=false,rejectPowerRead=false;inline int esp_wifi_get_max_tx_power(int8_t*out){*out=txPower;return rejectPowerRead?-1:0;}inline int esp_wifi_set_max_tx_power(int8_t value){if(rejectPower&&value==52)return -1;txPower=value;return 0;}
struct wifi_config_t{int preserved=123;};inline bool rejectConfig=false;inline int esp_wifi_get_config(int,wifi_config_t*out){out->preserved=123;return rejectConfig?-1:0;}
inline int esp_wifi_set_config(int,wifi_config_t*c){assert(c->preserved==123);c->preserved=456;return 0;}inline int esp_wifi_set_storage(int){return 0;}
constexpr int WIFI_PROTOCOL_11B=1,WIFI_PROTOCOL_11G=2,WIFI_PROTOCOL_11N=4,WIFI_PROTOCOL_11AX=64,WIFI_IF_STA=0;
enum wifi_phy_mode_t {WIFI_PHY_MODE_LR};
inline int esp_wifi_set_protocol(int,uint8_t value){if(failBg&&value==3)return -1;mask=value;return 0;}
inline int esp_wifi_get_protocol(int,uint8_t*out){*out=mask;return 0;}
inline int esp_wifi_sta_get_negotiated_phymode(wifi_phy_mode_t*out){*out=WIFI_PHY_MODE_LR;return 0;}
''')
    (root / 'HalPowerManager.h').write_text('''#pragma once
inline unsigned locks=0;struct HalPowerManager {struct Lock {Lock(){++locks;}~Lock(){--locks;}};};
''')
    (root / 'HalWifiAwakeLease.h').write_text('''#pragma once
inline unsigned leases=0;struct HalWifiAwakeLease {bool held=false;bool acquire(){held=true;++leases;return true;}~HalWifiAwakeLease(){if(held)--leases;}};
''')
    (root / 'lwip/sockets.h').write_text('''#pragma once
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>
#include <cassert>
#include "Arduino.h"
inline unsigned opened=0,closed=0, traceOpen=0;
inline bool firstTimeout=true;
inline int mockSocket(int,int,int){++opened;return opened;}
inline int mockConnect(int,const sockaddr*p,socklen_t){auto*a=(const sockaddr_in*)p;
 auto*b=(const uint8_t*)&a->sin_addr.s_addr;assert(b[0]==36&&b[1]==248&&b[2]==13&&(b[3]==186||b[3]==174));
 assert(a->sin_port==htons(443));errno=EINPROGRESS;return -1;}
inline int mockSelect(int nfds,fd_set*,fd_set*,fd_set*,timeval*t){now+=t->tv_usec/1000;return firstTimeout&&nfds==2?0:1;}
inline int mockGetsockopt(int,int,int,void*out,socklen_t*){*(int*)out=0;return 0;}
inline int mockClose(int){++closed;return 0;}
#define socket mockSocket
#define connect mockConnect
#define select mockSelect
#define getsockopt mockGetsockopt
#define close mockClose
''')
    (root / 'fcntl.h').write_text('#pragma once\nconstexpr int F_SETFL=4,O_NONBLOCK=4;inline int fcntl(int,int,int){return 0;}\n')
    (root / 'test.cpp').write_text('''#define RICKYOS_PLUS_RADIO_COMPARE 1
#include "production.cpp"
void plusTcpTraceBegin(){++traceOpen;}
void plusTcpTraceEnd(){--traceOpen;}
void reset(){now=0;mask=7;radioConnected=true;automatic=true;failBg=false;failFirstBgn=false;
 joins=bgnJoins=opened=closed=traceOpen=0;dnsResult=1;firstTimeout=true;}
int main(){
 reset();plusRadioCompareOnConnected();assert(opened==6&&closed==6&&traceOpen==0);
 assert(mask==7&&radioConnected&&automatic&&locks==0&&leases==0);
 assert(now>=50000&&now<60000);
 reset();dnsResult=-54;plusRadioCompareOnConnected();assert(opened==0&&joins==0&&automatic&&locks==0&&leases==0);
 reset();failBg=true;plusRadioCompareOnConnected();assert(opened==4&&closed==4&&mask==7&&radioConnected&&automatic);
 reset();failFirstBgn=true;plusRadioCompareOnConnected();assert(bgnJoins==2&&mask==7&&radioConnected&&automatic);
 assert(opened==4&&closed==4&&locks==0&&leases==0);
 reset();radioConnected=false;plusRadioCompareOnConnected();assert(opened==0&&leases==0&&locks==0);
}
''')
    subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined','-I'+str(root),str(root/'test.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
    code=(root/'test.cpp').read_text().replace('#define RICKYOS_PLUS_RADIO_COMPARE 1', '#define RICKYOS_PLUS_RADIO_COMPARE 1\n#define RICKYOS_PLUS_RADIO_TIMING 1')
    code=code[:code.index('int main(){')]+r'''int main(){
      reset();plusRadioCompareOnConnected();
      assert(opened==8&&closed==8&&joins==4&&bgnJoins==4);
      assert(now>=44000&&now<45000);
      assert(mask==7&&radioConnected&&automatic&&locks==0&&leases==0&&traceOpen==0);
      reset();dnsResult=-54;plusRadioCompareOnConnected();assert(opened==8&&joins==4&&automatic);
    }
    '''
    (root/'test.cpp').write_text(code)
    subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined','-I'+str(root),str(root/'test.cpp'),'-o',str(root/'timing')],check=True)
    subprocess.run([str(root/'timing')],check=True)
    rate=code[:code.index('int main(){')].replace('#define RICKYOS_PLUS_RADIO_TIMING 1', '#define RICKYOS_PLUS_RADIO_TIMING 1\n#define RICKYOS_PLUS_RADIO_RATE 1')
    rate+=r'''unsigned overrides=0;bool noAggregation=false;
    void plusRadioOverrideTxAggregation(bool value){++overrides;noAggregation=value;}
    int main(){
      reset();plusRadioCompareOnConnected();
      assert(opened==8&&closed==8&&joins==5&&overrides==5&&fixedCalls==2);
      assert(!noAggregation&&mask==7&&radioConnected&&automatic&&locks==0&&leases==0);
      reset();overrides=fixedCalls=0;rejectFixed=true;plusRadioCompareOnConnected();
      assert(opened==6&&closed==6&&joins==5&&overrides==5&&fixedCalls==1&&!noAggregation&&automatic);
      reset();rejectFixed=false;rejectConfig=true;plusRadioCompareOnConnected();
      assert(opened==0&&joins==0&&automatic&&locks==0&&leases==0);
    }
    '''
    (root/'test.cpp').write_text(rate)
    subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined','-I'+str(root),str(root/'test.cpp'),'-o',str(root/'rate')],check=True)
    subprocess.run([str(root/'rate')],check=True)
    power=rate[:rate.index('int main(){')].replace('#define RICKYOS_PLUS_RADIO_RATE 1', '#define RICKYOS_PLUS_RADIO_RATE 1\n#define RICKYOS_PLUS_RADIO_POWER 1')
    power+=r'''int main(){
      reset();overrides=fixedCalls=0;txPower=80;plusRadioCompareOnConnected();
      assert(opened==10&&closed==10&&joins==6&&overrides==6&&fixedCalls==0&&txPower==80);
      assert(!noAggregation&&mask==7&&radioConnected&&automatic&&locks==0&&leases==0);
      reset();overrides=fixedCalls=0;rejectPower=true;plusRadioCompareOnConnected();
      assert(opened==8&&closed==8&&joins==6&&txPower==80&&automatic);
      reset();rejectPower=false;rejectPowerRead=true;plusRadioCompareOnConnected();
      assert(opened==0&&joins==0&&automatic&&locks==0&&leases==0);
    }
    '''
    (root/'test.cpp').write_text(power)
    subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined','-I'+str(root),str(root/'test.cpp'),'-o',str(root/'power')],check=True)
    subprocess.run([str(root/'power')],check=True)
print('Actual radio comparison: same-IP sockets, bounded waits, failed joins/protocols, cleanup and restoration passed')
