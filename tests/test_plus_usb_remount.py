"""Verify USB-init failure remounts only the detached filesystem, never SDMMC."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
u=ROOT/'firmware/rickyos-plus-1.2/upstream'
s=(u/'freeink-sdk/libs/hardware/SDCardManager/src/SDCardManager.cpp').read_text()
a=s.index('bool SDCardManager::remountFilesystemAfterRawAccess()');b=s.index('\n#endif',a)
method=s[a:b]
code=r'''
#include <cassert>
#include <cstdint>
struct Dev{};
struct Volume {bool fail=false;int mounts=0;Dev* seen=nullptr;
 bool begin(Dev* dev){++mounts;seen=dev;return !fail;}};
uint64_t volumeTotalBytes(Volume&){return 123456;}
class SDCardManager {public: bool initialized=false,cachedUsedBytesValid=true;uint64_t cachedTotalBytes=0;
 Dev* _dev=nullptr;Volume _vol;Volume& vol(){return _vol;}
 bool remountFilesystemAfterRawAccess();};
'''+method+r'''
int main(){
 SDCardManager sd;assert(!sd.remountFilesystemAfterRawAccess() && !sd._vol.mounts);
 Dev dev;sd._dev=&dev;sd._vol.fail=true;assert(!sd.remountFilesystemAfterRawAccess() && !sd.initialized);
 sd._vol.fail=false;assert(sd.remountFilesystemAfterRawAccess());assert(sd._vol.seen==&dev);
 assert(sd.initialized && sd.cachedTotalBytes==123456 && !sd.cachedUsedBytesValid);
 int count=sd._vol.mounts;assert(sd.remountFilesystemAfterRawAccess() && sd._vol.mounts==count);
}
'''
with tempfile.TemporaryDirectory(prefix='plus-usb-remount-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
hal=(u/'lib/hal/HalStorage.cpp').read_text()
assert 'if (!SDCard.remountFilesystemAfterRawAccess())' in hal
print('Plus failed-USB startup remount: retained block device, no SDMMC reinitialization, failure/retry and cached state passed')
