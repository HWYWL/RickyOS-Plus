"""Execute production font connection/progress and WiFi retry/input edges without credentials or radio."""
from pathlib import Path
import tempfile,subprocess
r=Path(__file__).resolve().parents[1]/'firmware/rickyos-plus-1.2/upstream'
font=(r/'src/activities/settings/FontDownloadActivity.cpp').read_text()
wifi=(r/'src/activities/network/WifiSelectionActivity.cpp').read_text()
header=(r/'src/activities/network/WifiSelectionActivity.h').read_text()
constants='\n'.join(x for x in header.splitlines() if 'CONNECTION_TIMEOUT_MS =' in x)
a=wifi.index('  // Check for timeout',wifi.index('void WifiSelectionActivity::checkConnectionStatus()'))
b=wifi.index('\n}\n',a)+2
timeout='void WifiSelectionActivity::checkTimeout(){\n'+wifi[a:b]
def method(s,name):
 a=s.index(name);i=s.index('{',a)+1;depth=1
 while depth:
  if s[i]=='{':depth+=1
  elif s[i]=='}':depth-=1
  i+=1
 return s[a:i]
helper=font[font.index('constexpr uint32_t kDownloadProgressRefreshMs'):font.index('std::atomic<bool> chineseFontPromptShownThisBoot')]
loop=wifi[wifi.index('void WifiSelectionActivity::loop()'):wifi.index('  switch (state) {',wifi.index('void WifiSelectionActivity::loop()'))]+'  ++manualInputs;\n}\n'
# The entry guard is copied verbatim up to the first unrelated startup operation.
entry=wifi[wifi.index('void WifiSelectionActivity::onEnter()'):wifi.index('  NetworkStartup::logMemory("WifiSelectionActivity enter")')]+ '  ++prepared;\n}\n'
code=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#define LOG_DBG(...) ((void)0)
inline uint32_t now=0;uint32_t millis(){return now;}
constexpr int WL_CONNECTED=3;
struct {std::string toString(){return "192.0.2.1";}}ip;
struct {std::string SSID(){return "test-ap";}auto localIP(){return ip;}int statusValue=0,disconnects=0;int status(){return statusValue;}void disconnect(){++disconnects;}}WiFi;
struct ActivityResult{bool isCancelled=false;};
struct Activity{void onEnter(){}};
struct RenderLock{inline static bool held=false;template<class T>RenderLock(T&){assert(!held);held=true;}~RenderLock(){held=false;}};
struct MappedInputManager{enum class Button{Confirm};bool pressed=false;bool isPressed(Button){return pressed;}};
enum class WifiSelectionState{CONNECTING,AUTO_CONNECTING,SCANNING,NETWORK_LIST,CONNECTION_FAILED};
constexpr int STR_ERROR_CONNECTION_TIMEOUT=0;const char*tr(int){return "timeout";}
struct WifiSelectionActivity:Activity {
 MappedInputManager mappedInput;bool allowAutoConnect=true,inheritedConfirmPending=false;
 bool autoConnecting=true;int completed=0,prepared=0,manualInputs=0,checks=0,scanChecks=0,scanStarts=0,refreshes=0,nextAttempts=0;
 WifiSelectionState state=WifiSelectionState::AUTO_CONNECTING;
 std::vector<int> networks;std::vector<std::string> autoAttemptedSsids{"saved"};std::string selectedSSID,connectedIP;
 int selectedNetworkIndex=0;
 uint32_t connectionStartTime=0;std::string connectionError;
 CONSTANTS
 void checkTimeout();void onEnter();void loop();void handleAutoConnectFailure();
 void onComplete(bool ok){assert(ok);++completed;}
 void checkConnectionStatus(){++checks;}void processWifiScanResults(){++scanChecks;}
 bool tryNextSavedNetworkFromScan(){++nextAttempts;return false;}
 void requestUpdate(){++refreshes;}void startWifiScan(bool automatic){assert(automatic);++scanStarts;autoConnecting=true;}
};
struct FontDownloadActivity {
 enum class Purpose{Manage,ReaderAutoInstall};enum class ExitRoute{ReaderSuppressPrompt};
 Purpose purpose_=Purpose::Manage;
 size_t downloadProgress_=0,downloadTotal_=0,lastProgressRefreshBytes_=0;
 char downloadSourceHost_[256]{}; uint32_t lastProgressRefreshMs_=0;int frames=0,selectors=0,connected=0,waitFrames=0;
 void resetDownloadProgress(size_t);void updateDownloadProgress(size_t);void finishDownloadProgress();void startWifiSelection();
 void requestUpdate(bool immediate){assert(immediate && !RenderLock::held);++frames;}
 void requestUpdateAndWait(){assert(!RenderLock::held);++waitFrames;}
 void onWifiSelectionComplete(bool success){assert(success);++connected;}
 template<class T,class F>bool startActivityForResultWith(F){++selectors;return true;}
 void finishAutomaticFlow(ExitRoute){}void finish(){}
};
'''.replace('CONSTANTS',constants)+helper+entry+loop+method(wifi,'void WifiSelectionActivity::handleAutoConnectFailure(')+timeout
for sig in ['void FontDownloadActivity::startWifiSelection(', 'void FontDownloadActivity::resetDownloadProgress(', 'void FontDownloadActivity::updateDownloadProgress(', 'void FontDownloadActivity::finishDownloadProgress(']:code+='\n'+method(font,sig)
code+=r'''
int main(){
 FontDownloadActivity f;
 WiFi.statusValue=WL_CONNECTED;f.startWifiSelection();assert(f.connected==1 && f.selectors==0);
 WiFi.statusValue=0;f.startWifiSelection();assert(f.selectors==1);
 now=0;f.resetDownloadProgress(10*1024*1024);
 now=1999;f.updateDownloadProgress(4096);assert(f.frames==0);
 now=2000;f.updateDownloadProgress(8192);assert(f.frames==1 && downloadProgressPercent(f.downloadProgress_,f.downloadTotal_)==0);
 now=4000;f.updateDownloadProgress(16384);assert(f.frames==2);
 now=6000;f.updateDownloadProgress(16384);assert(f.frames==2);
 f.updateDownloadProgress(20000);assert(f.frames==3);
 now=8000;f.updateDownloadProgress(20*1024*1024);assert(f.downloadProgress_==f.downloadTotal_ && f.frames==3);
 f.finishDownloadProgress();assert(f.waitFrames==1);
 now=0xfffffff0;f.resetDownloadProgress(1024*1024);now=1984;f.updateDownloadProgress(4096);assert(f.frames==4);
 WifiSelectionActivity w;WiFi.statusValue=WL_CONNECTED;w.mappedInput.pressed=true;w.onEnter();assert(w.completed==1 && w.prepared==0 && w.selectedSSID=="test-ap" && w.connectedIP=="192.0.2.1");
 w.loop();assert(w.checks==1 && w.manualInputs==0);
 w.mappedInput.pressed=false;w.loop();assert(!w.inheritedConfirmPending && w.manualInputs==0);
 w.loop();assert(w.manualInputs==1);
 WiFi.statusValue=0;WifiSelectionActivity off;off.onEnter();assert(off.completed==0 && off.prepared==1);
 off.handleAutoConnectFailure();assert(off.autoAttemptedSsids.empty() && off.scanStarts==1);
 off.networks.push_back(1);off.autoAttemptedSsids.push_back("saved");off.handleAutoConnectFailure();
 assert(off.autoAttemptedSsids.size()==1 && off.scanStarts==1 && !off.autoConnecting && off.state==WifiSelectionState::NETWORK_LIST);
 WifiSelectionActivity timed;timed.connectionStartTime=3000;now=10000;
 const int disconnects=WiFi.disconnects;timed.checkTimeout();assert(WiFi.disconnects==disconnects);
 now=18001;timed.checkTimeout();assert(WiFi.disconnects==disconnects+2 && timed.scanStarts==1);
 timed.autoConnecting=false;timed.state=WifiSelectionState::CONNECTING;timed.checkTimeout();
 assert(timed.state==WifiSelectionState::CONNECTION_FAILED);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Font reconnect/progress: reuse connected station, saved retry bounded, inherited Confirm consumed, sub-1% byte progress, 2-second throttling, completion and wraparound passed')
