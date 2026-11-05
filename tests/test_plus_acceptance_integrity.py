"""Real integrity/temp reservation helpers against known CRC and exclusive POSIX files."""
from pathlib import Path
import subprocess,tempfile
source=Path('firmware/rickyos-plus-1.2/components/rickyos_application/port/PlusNetworkAcceptance.cpp').read_text()
def extract(name):
 start=source.index(name);body=source.index('{',start);depth=1;end=body+1
 while depth:
  if source[end]=='{':depth+=1
  elif source[end]=='}':depth-=1
  end+=1
 return source[start:end]
with tempfile.TemporaryDirectory() as d:
 root=Path(d)
 code='''#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <string>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#define O_WRITE O_WRONLY
std::string base;
struct File {int fd;~File(){if(fd>=0)::close(fd);}operator bool()const{return fd>=0;}};
struct Store {File open(const char*p,int flags){return {::open((base+"/"+std::string(p).substr(std::string(p).find_last_of('/')+1)).c_str(),flags,0600)};}} Storage;
'''+extract('uint32_t updateCrc')+'\n'+extract('bool reserveTemp')+'''
int main(int argc,char**argv){base=argv[1];
 const uint8_t text[]="123456789";
 assert((updateCrc(0xffffffffu,text,9)^0xffffffffu)==0xcbf43926u);
 auto crc=updateCrc(0xffffffffu,text,4);crc=updateCrc(crc,text+4,5);assert((crc^0xffffffffu)==0xcbf43926u);
 std::string original=base+"/plus-net-accept-20261016-unit-0.tmp";{std::ofstream f(original);f<<"sentinel";}
 char path[96];assert(reserveTemp("unit",path,sizeof(path)));
 assert(std::string(path).ends_with("unit-1.tmp"));
 std::string value;{std::ifstream f(original);f>>value;}assert(value=="sentinel");
 char small[8];assert(!reserveTemp("unit",small,sizeof(small)));
}
'''
 (root/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined',str(root/'test.cpp'),'-o',str(root/'test')],check=True)
 subprocess.run([str(root/'test'),str(root)],check=True)
print('Actual acceptance CRC known/chunked vectors and exclusive temporary-file preservation passed')
