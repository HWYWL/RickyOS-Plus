"""Execute actual Plus MSC callbacks/lifecycle with fake USB and sector device."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
PORT=ROOT/'firmware/rickyos-plus-1.2'
header=(PORT/'components/rickyos_storage/include/UsbMassStorage.h').read_text()
header='\n'.join(x for x in header.splitlines() if not x.startswith('#include') and x!='#pragma once')
source=(PORT/'components/rickyos_storage/port/PlusUsbMassStorage.cpp').read_text()
source='\n'.join(x for x in source.splitlines() if not x.startswith('#include'))
shim=r'''
#include <cassert>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <vector>
#define FREEINK_CAP_USB_MSC 1
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
using Sector_t=uint32_t;
struct StaticSemaphore_t {bool held=false;};using SemaphoreHandle_t=StaticSemaphore_t*;
constexpr int pdTRUE=1,portMAX_DELAY=-1;
#define pdMS_TO_TICKS(x) x
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* p){return p;}
int xSemaphoreTake(SemaphoreHandle_t p,int){assert(!p->held);p->held=true;return pdTRUE;}
void xSemaphoreGive(SemaphoreHandle_t p){assert(p->held);p->held=false;}

struct FsBlockDeviceInterface {
 std::vector<uint8_t> data=std::vector<uint8_t>(8*512,0x5a);
 bool failRead=false,failWrite=false,failSync=false;int reads=0,writes=0,syncs=0;
 uint32_t sectorCount(){return 8;}
 bool readSectors(uint32_t s,void* p,size_t n){++reads;if(failRead)return false;assert(s+n<=8);memcpy(p,data.data()+s*512,n*512);return true;}
 bool writeSectors(uint32_t s,const void* p,size_t n){++writes;if(failWrite)return false;assert(s+n<=8);memcpy(data.data()+s*512,p,n*512);return true;}
 bool readSector(uint32_t s,void* p){return readSectors(s,p,1);}
 bool writeSector(uint32_t s,const void* p){return writeSectors(s,p,1);}
 bool syncDevice(){++syncs;return !failSync;}
};
using esp_err_t=int;constexpr int ESP_OK=0;
const char* esp_err_to_name(int){return "fake";}
int installError=0,uninstallError=0,reboots=0;bool mounted=false,suspended=false,routeDebug=true,installed=false;
int sense=0;std::vector<int> events;
uint32_t clockMs=1000;uint8_t vbusState=1;int readStatusError=0;
uint32_t millis(){clockMs+=300;return clockMs;}
bool metalio_i2c1_lock(int){return true;}void metalio_i2c1_unlock(){}
int plus_i2c_read(int addr,int reg,uint8_t* value,int size){assert(addr==0x6b && reg==0x1e && size==1);*value=vbusState;return readStatusError;}

void epd_board_metalio_eink4_plus_ana_sw_set(bool debug){routeDebug=debug;events.push_back(debug?3:2);}
constexpr int ESP_MAC_WIFI_STA=0;
int esp_read_mac(uint8_t* mac,int type){assert(type==ESP_MAC_WIFI_STA);memset(mac,0xab,6);return 0;}
void esp_restart(){++reboots;}
bool tud_mounted(){return mounted;}bool tud_suspended(){return suspended;}
bool tud_disconnect(){mounted=false;events.push_back(4);return true;}
bool tud_connect(){events.push_back(6);return true;}
void delay(int n){assert(n==150);}
void tud_msc_set_sense(uint8_t,int value,int,int){sense=value;}
constexpr int SCSI_SENSE_NOT_READY=2,SCSI_SENSE_ILLEGAL_REQUEST=5;
constexpr int TUSB_DESC_DEVICE=1,CFG_TUD_ENDPOINT0_SIZE=64;
#define TUD_CONFIG_DESCRIPTOR(...) 1,2
#define TUD_MSC_DESCRIPTOR(...) 3,4
struct tusb_desc_device_t {uint8_t bLength,bDescriptorType;uint16_t bcdUSB;uint8_t bMaxPacketSize0;uint16_t idVendor,idProduct,bcdDevice;uint8_t iManufacturer,iProduct,iSerialNumber,bNumConfigurations;};
struct tinyusb_config_t {struct Descriptor{const tusb_desc_device_t* device=nullptr;const char** string=nullptr;int string_count=0;const uint8_t *full_speed_config=nullptr,*high_speed_config=nullptr;}descriptor;};
#define TINYUSB_DEFAULT_CONFIG() tinyusb_config_t{}
int tinyusb_driver_install(const tinyusb_config_t* c){
 assert(routeDebug);assert(c->descriptor.device->idVendor==0x303a && c->descriptor.device->idProduct==0x4002);
 assert(c->descriptor.string_count==4 && strlen(c->descriptor.string[3])==12);
 assert(c->descriptor.full_speed_config && c->descriptor.high_speed_config);
 events.push_back(1);installed=!installError;return installError;
}
bool freeink_dummy_gate();
int tinyusb_driver_uninstall(){assert(installed);assert(freeink_dummy_gate());events.push_back(5);if(!uninstallError)installed=false;return uninstallError;}
'''
main=r'''
int main(){
 using namespace freeink;UsbMassStorage usb;FsBlockDeviceInterface dev;
 assert(!usb.begin(nullptr));installError=1;assert(!usb.begin(&dev));assert(!usb.active() && routeDebug && !gDev.load());
 installError=0;events.clear();assert(usb.begin(&dev));assert((events==std::vector<int>{1,4,2,6}));assert(!usb.begin(&dev));
 assert(usb.state()==UsbMassStorageState::WaitingForHost);mounted=true;assert(usb.hostConnected());
 uint32_t blocks=0;uint16_t size=0;tud_msc_capacity_cb(0,&blocks,&size);assert(blocks==8&&size==512);
 assert(tud_msc_test_unit_ready_cb(0));assert(!tud_msc_test_unit_ready_cb(1));assert(sense==SCSI_SENSE_NOT_READY);
 uint8_t payload[600];memset(payload,0xc3,sizeof(payload));
 assert(tud_msc_write10_cb(0,1,100,payload,600)==600);
 assert(dev.data[611]==0x5a && dev.data[612]==0xc3 && dev.data[1211]==0xc3 && dev.data[1212]==0x5a);
 uint8_t readback[600];assert(tud_msc_read10_cb(0,1,100,readback,600)==600 && !memcmp(payload,readback,600));
 uint8_t block[512];assert(tud_msc_read10_cb(0,0,0,block,512)==512);
 assert(usb.state()==UsbMassStorageState::Accessed);
 uint8_t command[16]={0x35};assert(tud_msc_scsi_cb(0,command,nullptr,0)==0);
 command[0]=0x91;int beforeSync=dev.syncs;assert(tud_msc_scsi_cb(0,command,nullptr,0)==0 && dev.syncs==beforeSync+1);
 command[0]=0x5a;uint8_t mode[16];memset(mode,0xcc,sizeof(mode));
 assert(tud_msc_scsi_cb(0,command,mode,sizeof(mode))==8 && mode[0]==0 && mode[1]==6 && mode[7]==0 && mode[8]==0xcc);
 memset(mode,0xcc,sizeof(mode));assert(tud_msc_scsi_cb(0,command,mode,4)==4 && mode[4]==0xcc);
 assert(tud_msc_scsi_cb(0,command,nullptr,0)==0 && tud_msc_scsi_cb(1,command,mode,8)==-1);

 dev.failSync=true;assert(!tud_msc_start_stop_cb(0,0,false,true));assert(usb.state()==UsbMassStorageState::IoError);
 dev.failSync=false;usb.end();assert(!usb.active() && !installed && routeDebug && !gDev.load());
 assert(events[events.size()-3]==4 && events[events.size()-2]==5 && events.back()==3);
 assert(usb.begin(&dev));mounted=true;assert(tud_msc_start_stop_cb(0,0,false,true));assert(usb.state()==UsbMassStorageState::Ejected);
 assert(!tud_msc_test_unit_ready_cb(0) && !tud_msc_is_writable_cb(0));assert(tud_msc_write10_cb(0,0,0,block,512)==-1);usb.end();
 assert(usb.begin(&dev));mounted=true;
 assert(tud_msc_read10_cb(0,UINT32_MAX,UINT32_MAX,block,512)==-1);
 assert(usb.state()==UsbMassStorageState::IoError);usb.end();
 assert(usb.begin(&dev));mounted=true;dev.failRead=true;assert(tud_msc_read10_cb(0,0,0,block,512)==-1);dev.failRead=false;usb.end();
 assert(usb.begin(&dev));mounted=true;dev.failWrite=true;assert(tud_msc_write10_cb(0,0,0,block,512)==-1);dev.failWrite=false;
 uninstallError=1;usb.end();assert(reboots==1 && !usb.active() && gDev.load()==&dev && routeDebug);
 uninstallError=0;freeink::gOwner.store(nullptr);freeink::gDev.store(nullptr);installed=false; // Simulated restart resets static state.
 assert(!gDev.load());
 assert(usb.begin(&dev));mounted=true;assert(usb.hostConnected());suspended=true;assert(!usb.hostSuspended()); // Host sleep with VBUS must not reclaim SD.
 vbusState=0;assert(usb.hostSuspended());readStatusError=1;assert(!usb.hostSuspended());readStatusError=0;
 mounted=false;assert(usb.state()==UsbMassStorageState::Disconnected);usb.end();
 assert(tud_msc_read10_cb(0,0,0,block,512)==-1);assert(!tud_msc_is_writable_cb(0));
 command[0]=0xff;assert(tud_msc_scsi_cb(0,command,nullptr,0)==-1 && sense==SCSI_SENSE_ILLEGAL_REQUEST);
}
'''
with tempfile.TemporaryDirectory(prefix='plus-usb-tests-') as folder:
 p=Path(folder);(p/'test.cpp').write_text(shim+header+source+'\nbool freeink_dummy_gate(){return freeink::callbackMutex->held;}\n'+main)
 subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Plus USB actual backend: sector bounds/partial I/O, flush/eject, lifecycle and failure cleanup passed (ASan/UBSan)')
