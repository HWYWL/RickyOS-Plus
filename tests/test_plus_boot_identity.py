"""Same dev version must prepare once per image, not once for all future builds."""
from pathlib import Path
import subprocess
import tempfile

source = Path('firmware/rickyos-plus-1.2/upstream/src/util/BootPreparation.cpp').read_text()
body = source[source.index('void firmwareIdentity('):source.index('\nbool libraryNeedsBuild(')]
program = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#define FREEINK_DEVICE_METALIO_PLUS 1
#define CROSSPOINT_VERSION "1.2.0"
constexpr char kPreparedMarker[]="/marker";
std::string marker;
struct HalFile {
 int read(uint8_t* out,size_t n){auto size=std::min(n,marker.size());memcpy(out,marker.data(),size);return size;}
 size_t write(const uint8_t* data,size_t n){marker.assign(reinterpret_cast<const char*>(data),n);return n;}
};
struct Store {
 bool openFileForRead(const char*,const char*,HalFile&){return !marker.empty();}
 bool openFileForWrite(const char*,const char*,HalFile&){return true;}
} Storage;
namespace HalSystem {
 std::array<uint8_t,8> id{};
 std::array<uint8_t,8> getFirmwareBuildId(){return id;}
}
''' + body + r'''
int main(){
 assert(!preparedForThisFirmware());
 markPrepared();assert(preparedForThisFirmware());
 const auto first=marker;markPrepared();assert(marker==first);
 HalSystem::id[7]=0xff;assert(!preparedForThisFirmware());
 markPrepared();assert(preparedForThisFirmware() && marker!=first);
 marker=CROSSPOINT_VERSION;assert(!preparedForThisFirmware());
 char identity[64];firmwareIdentity(identity);assert(strlen(identity)<64);
 assert(std::string(identity).ends_with("ff"));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp);(p/'test.cpp').write_text(program)
    subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('Plus preparation marker: same image stable, same-version new image invalidates, legacy marker migrates, bounded identity passed')
