"""Actual unattended bootstrap: visible saved choices, fallback and no credential writes."""
from pathlib import Path
import subprocess,tempfile
source=Path('firmware/rickyos-plus-1.2/components/rickyos_application/port/PlusRadioAuto.cpp')
with tempfile.TemporaryDirectory() as directory:
 root=Path(directory);(root/'production.cpp').write_bytes(source.read_bytes())
 (root/'Arduino.h').write_text('#pragma once\n#include <cstdint>\ninline uint32_t now=0;inline uint32_t millis(){return now;}inline void delay(unsigned n){now+=n;}\n')
 (root/'Logging.h').write_text('#pragma once\n#define LOG_INF(...) ((void)0)\n#define LOG_ERR(...) ((void)0)\n')
 (root/'PlusNetAllocTrace.h').write_text('#pragma once\n')
 (root/'WiFi.h').write_text('''#pragma once
#include <string>
#include <cassert>
inline bool connected=false,scanFreed=false,visible=true;
inline unsigned begins=0,scans=0;
inline std::string selected;
constexpr int WL_CONNECTED=3,WIFI_STA=1,WIFI_ALL_CHANNEL_SCAN=1,WIFI_CONNECT_AP_BY_SIGNAL=1;
struct Wifi {
 int STA=1;void disconnect(bool,bool,unsigned=0){connected=false;}
 void setScanMethod(int){}void setSortMethod(int){}
 int scanNetworks(bool,bool){++scans;scanFreed=false;return visible?2:0;}
 std::string SSID(int i){return i==0?"weak":"strong";}
 int32_t RSSI(int i){return i==0?-70:-45;}
 void scanDelete(){scanFreed=true;}
 void begin(const char*s,const char*p){assert(scanFreed);selected=s;++begins;connected=std::string(p)=="ok";}
 int status(){return connected?3:6;}
};inline Wifi WiFi;
''')
 (root/'esp_wifi.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,WIFI_IF_STA=0,WIFI_PROTOCOL_11B=1,WIFI_PROTOCOL_11G=2,WIFI_PROTOCOL_11N=4;inline int esp_wifi_set_protocol(int,int){return 0;}\n')
 (root/'Network.h').write_text('#pragma once\nstruct Net{bool setDefaultInterface(int){return true;}};inline Net Network;\n')
 (root/'HalWifiDiagnostics.h').write_text('#pragma once\ninline void plusWifiDiagnosticsBegin(){}\n')
 (root/'HalPowerManager.h').write_text('#pragma once\ninline int locks=0;struct HalPowerManager{struct Lock{Lock(){++locks;}~Lock(){--locks;}};};\n')
 (root/'NetworkStartup.h').write_text('#pragma once\nstruct GfxRenderer{};namespace NetworkStartup{inline bool setMode(GfxRenderer&,int){return true;}}\n')
 (root/'WifiCredentialStore.h').write_text('''#pragma once
#include <optional>
#include <string>
inline unsigned saved=2,loads=0;
inline bool strongWorks=true;
struct WifiCredential{std::string ssid,password;};
struct Store{
 void loadFromFile(){++loads;}
 size_t getCredentialCount(){return saved;}
 std::optional<std::string> getSsidAt(unsigned i){return i==0?"weak":"strong";}
 std::optional<WifiCredential> getCredentialAt(unsigned i){return WifiCredential{i==0?"weak":"strong",(i==0||strongWorks)?"ok":"bad"};}
};inline Store store;
#define WIFI_STORE store
''')
 (root/'test.cpp').write_text('''#define RICKYOS_PLUS_RADIO_AUTOSTART 1
#include "production.cpp"
inline unsigned comparisons=0;
void plusRadioCompareOnConnected(){assert(locks==0);++comparisons;}
void reset(){now=0;begins=scans=comparisons=loads=0;saved=2;visible=strongWorks=true;connected=false;scanFreed=false;selected.clear();}
int main(){GfxRenderer renderer;
 reset();plusRadioAutoStart(renderer);assert(comparisons==1&&selected=="strong"&&begins==1&&scanFreed&&locks==0);
 reset();strongWorks=false;plusRadioAutoStart(renderer);assert(comparisons==1&&selected=="weak"&&begins==2&&now==35000&&locks==0);
 reset();visible=false;plusRadioAutoStart(renderer);assert(scans==2&&begins==0&&comparisons==0&&now==5000&&locks==0);
 reset();saved=0;plusRadioAutoStart(renderer);assert(scans==0&&begins==0&&comparisons==0&&loads==1&&locks==0);
}
''')
 subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined','-I'+str(root),str(root/'test.cpp'),'-o',str(root/'test')],check=True)
 subprocess.run([str(root/'test')],check=True)
 # A private test profile must never load/fall back to saved networks, even on
 # association failure. The actual secret-bearing header is never used here.
 (root/'PlusPrivateNetworkProfile.h').write_text('''#pragma once
inline bool privateConnects=true;
static bool plusPrivateNetworkBootstrap(GfxRenderer&){connected=privateConnects;return connected;}
''')
 (root/'private-test.cpp').write_text('''#define RICKYOS_PLUS_RADIO_AUTOSTART 1
#define RICKYOS_PLUS_NET_PRIVATE_PROFILE 1
#include "production.cpp"
inline unsigned comparisons=0;
void plusRadioCompareOnConnected(){assert(locks==0);++comparisons;}
int main(){GfxRenderer renderer;
 plusRadioAutoStart(renderer);
 assert(comparisons==1&&loads==0&&scans==0&&begins==0&&locks==0);
 privateConnects=false;comparisons=0;plusRadioAutoStart(renderer);
 assert(comparisons==0&&loads==0&&scans==0&&begins==0&&locks==0);
}
''')
 subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined','-I'+str(root),
                 str(root/'private-test.cpp'),'-o',str(root/'private-test')],check=True)
 subprocess.run([str(root/'private-test')],check=True)
print('Actual radio auto bootstrap: strongest visible saved profile, fallback, unavailable network and non-nested power locks passed')
