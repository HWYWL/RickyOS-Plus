"""Run the actual offline recorder with USB ownership and SD failures injected."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'firmware/rickyos-plus-1.2'
source = (PORT / 'components/rickyos_application/port/PlusFieldDiagnostics.cpp').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
SHIM = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <sstream>
#include <vector>
uint32_t tick=0;
unsigned opens=0,closes=0,samples=0,writes=0;
bool busy=false,openFail=false,shortWrite=false,closeFail=false,locked=false;
std::string contents;
uint32_t millis(){return tick;}
unsigned getCpuFrequencyMhz(){return 80;}
enum class UsbDriveState {Unsupported,WaitingForHost,Connected,Ejected,Disconnected,IoError};
UsbDriveState usb=UsbDriveState::Unsupported;
constexpr int O_WRONLY=1,O_CREAT=2,O_APPEND=4,ESP_OK=0;
struct File {
 bool active;
 explicit operator bool()const{return active;}
 size_t fileSize()const{return contents.size();}
 size_t write(const void* data,size_t count){
  assert(active&&locked&&usb==UsbDriveState::Unsupported);++writes;
  const size_t actual=shortWrite?count-1:count;
  contents.append(static_cast<const char*>(data),actual);return actual;
 }
 bool close(){assert(active);active=false;++closes;tick+=2;return !closeFail;}
 ~File(){if(active)close();}
};
struct {
 UsbDriveState usbDriveState(){return usb;}
 File open(const char* path,int flags){assert(locked);assert(!strcmp(path,"/.crosspoint/plus-display-memory-v1.csv"));
  assert(flags==(O_WRONLY|O_CREAT|O_APPEND));++opens;return {!openFail};}
} Storage;
struct RenderLock {
 enum class Mode {Try};
 bool held;
 explicit RenderLock(Mode){held=!busy;if(held){assert(!locked);locked=true;}}
 bool ownsLock()const{return held;}
 ~RenderLock(){if(held)locked=false;}
};
namespace HalMemory {
struct Stats {size_t freeBytes,totalBytes,minFreeBytes,largestBlockBytes;};
Stats getInternalHeap(){return {100,300,80,40};}
Stats getInternalDmaHeap(){return {60,200,50,30};}
Stats getPsramHeap(){return {10000,16000,9000,8000};}
}
struct BatteryMonitor {
 struct PowerSnapshot {uint16_t mv=4200;int16_t current=-150,average=-140;bool mvOk=true,currentOk=true,averageOk=false;};
 PowerSnapshot readPowerSnapshot()const{assert(locked);++samples;return {};}
};
bool metalio_i2c1_lock(int wait){assert(wait==20);return true;}
void metalio_i2c1_unlock(){}
int plus_i2c_read(int addr,int reg,uint8_t* value,int count){assert(addr==0x6b&&reg==0x1e&&count==1);*value=0;return -1;}
bool epd_board_metalio_eink4_plus_tps_ok(){return true;}
uint32_t epd_lcd_get_first_fault(){return 0x12340001;}
extern "C" void epd_lcd_get_timing(int* a,int* b,int* c){*a=13000;*b=12000;*c=14000;}
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
'''
MAIN = r'''
void step(){tick+=5000;plusFieldDiagnosticsPoll();assert(!locked);}
int main(int argc,char** argv){
 assert(argc==2);std::string mode=argv[1];
 if(mode=="usb"){
  for(auto state:{UsbDriveState::WaitingForHost,UsbDriveState::Connected,UsbDriveState::Ejected,
                  UsbDriveState::Disconnected,UsbDriveState::IoError}){usb=state;step();}
  assert(opens==0&&samples==0);return 0;
 }
 if(mode=="busy"){busy=true;step();assert(opens==0&&samples==0);busy=false;step();assert(opens==1);return 0;}
 if(mode=="open-fail"){openFail=true;step();step();assert(opens==1&&writes==0);return 0;}
 if(mode=="short-write"){shortWrite=true;step();step();assert(opens==1&&closes==1&&writes==1);return 0;}
 if(mode=="close-fail"){closeFail=true;step();step();assert(opens==1&&closes==1);return 0;}
 if(mode=="full"){
  contents=std::string(128*1024,'x');step();step();
  assert(opens==1&&closes==1&&writes==0&&contents==std::string(128*1024,'x'));return 0;
 }
 if(mode=="wrap"){
  lastAt=UINT32_MAX-1000;tick=1000;plusFieldDiagnosticsPoll();assert(opens==0);
  tick=5000;plusFieldDiagnosticsPoll();assert(opens==1);return 0;
 }
 assert(mode=="normal");contents="existing data\n";
 tick=4999;plusFieldDiagnosticsPoll();assert(samples==0&&opens==0);
 step();assert(opens==1&&closes==1&&samples==1);
 auto first=contents;plusFieldDiagnosticsPoll();assert(contents==first&&opens==1);
 step();assert(opens==2&&closes==2&&writes==3);
 assert(contents.rfind("existing data\nup_ms,",0)==0); // Never truncates existing content.
 assert(contents.find("up_ms,",contents.find("up_ms,")+1)==std::string::npos);
 std::istringstream lines(contents);std::string line;
 std::getline(lines,line);std::getline(lines,line);
 std::vector<std::string> header;{std::istringstream in(line);std::string v;while(std::getline(in,v,','))header.push_back(v);}
 assert(header.size()==24);
 std::getline(lines,line);std::istringstream in(line);std::string value;std::vector<std::string> row;
 while(std::getline(in,value,','))row.push_back(value);
 assert(row.size()==header.size());
 assert(row[1]=="4200"&&row[3]=="-150"&&row[5]=="-140"&&row[6]=="0"&&row[8]=="-1");
 assert(row[14]=="100"&&row[15]=="80"&&row[16]=="40"&&row[17]=="60"&&row[20]=="10000");
}
'''
with tempfile.TemporaryDirectory(prefix='plus-field-recorder-') as folder:
    path = Path(folder)
    (path / 'test.cpp').write_text(SHIM + source + MAIN)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-DRICKYOS_PLUS_FIELD_DIAG=1',
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    for case in ('normal', 'usb', 'busy', 'open-fail', 'short-write', 'close-fail', 'full', 'wrap'):
        subprocess.run([str(path / 'test'), case], check=True)
    # Ordinary builds do not reference any diagnostic API, state or SDK helper.
    (path / 'off.cpp').write_text(source)
    subprocess.run(['clang++', '-std=c++17', '-DRICKYOS_PLUS_FIELD_DIAG=0', '-c',
                    str(path / 'off.cpp'), '-o', str(path / 'off.o')], check=True)
    assert 'plusFieldDiagnosticsPoll' not in subprocess.check_output(['nm', str(path / 'off.o')], text=True)
print('Actual offline recorder: bounded append, USB exclusion, nonblocking render gate, '
      'I/O failure stop, wraparound, unknown flags and default-off passed (ASan/UBSan)')
