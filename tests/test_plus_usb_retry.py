"""Execute the imported block retry policy and its accounting without a USB/card."""
from pathlib import Path
import subprocess,tempfile
u=Path('firmware/rickyos-plus-1.2/upstream')
s=(u/'lib/hal/HalStorage.cpp').read_text();body=s[s.index('class RetryingBlockDevice'):s.index('\nRetryingBlockDevice usbBlockDevice;')]
h=(u/'lib/hal/HalStorage.h').read_text();stats=h[h.index('struct UsbDriveStats'):h.index('\nclass HalStorage')]
program=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <initializer_list>
#include <cstring>
template<class... Args> void log(Args...) {}
#define LOG_ERR(...) log(__VA_ARGS__)
using Sector_t=uint32_t;
uint32_t now=10;uint32_t millis(){return now;}void delay(uint32_t ms){now+=ms;}
struct FsBlockDeviceInterface {
 virtual ~FsBlockDeviceInterface()=default;
 virtual void end()=0;virtual bool isBusy()=0;virtual Sector_t sectorCount()=0;virtual bool syncDevice()=0;
 virtual bool readSector(Sector_t,uint8_t*)=0;virtual bool readSectors(Sector_t,uint8_t*,size_t)=0;
 virtual bool writeSector(Sector_t,const uint8_t*)=0;virtual bool writeSectors(Sector_t,const uint8_t*,size_t)=0;
};
''' + stats + body + r'''
struct Device:FsBlockDeviceInterface {
 int calls=0,failuresLeft=0,ends=0;bool busy=false;
 bool op(){++calls;now+=12;if(failuresLeft){--failuresLeft;return false;}return true;}
 void end()override{++ends;}bool isBusy()override{return busy;}Sector_t sectorCount()override{return 1024;}
 bool syncDevice()override{return true;}
 bool readSector(Sector_t,uint8_t*)override{return op();}bool readSectors(Sector_t,uint8_t*,size_t)override{return op();}
 bool writeSector(Sector_t,const uint8_t*)override{return op();}bool writeSectors(Sector_t,const uint8_t*,size_t)override{return op();}
};
int main(){
 Device dev;RetryingBlockDevice retry;retry.attach(&dev);uint8_t bytes[512]{};
 dev.failuresLeft=2;assert(retry.readSector(1,bytes) && dev.calls==3);
 auto stats=retry.stats();assert(stats.reads==1 && stats.retries==2 && stats.failures==0 && stats.maxOpMs==12 && stats.inFlightMs==0);
 dev.failuresLeft=4;int before=dev.calls;assert(!retry.writeSectors(4,bytes,1) && dev.calls==before+4);
 stats=retry.stats();assert(stats.writes==1 && stats.retries==6 && stats.failures==1 && stats.sectorsWritten==0);
 assert(retry.writeSectors(4,bytes,1));assert(retry.stats().sectorsWritten==1);
 retry.resetStats();stats=retry.stats();assert(stats.reads==0 && stats.writes==0 && stats.retries==0 && stats.failures==0);
 assert(retry.syncDevice() && retry.sectorCount()==1024 && !retry.isBusy());retry.end();assert(dev.ends==1);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(program)
 subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual USB block retry: bounded transient/permanent failure, correct successful-write accounting, timing/reset and delegation passed')
