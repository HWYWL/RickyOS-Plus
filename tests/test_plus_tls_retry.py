"""Actual SecureClient connection policy: TLS fallback cannot repeat DNS/TCP failure."""
from pathlib import Path
import subprocess,tempfile
p=Path('firmware/rickyos-plus-1.2/upstream/freeink-sdk/libs/network/SecureNet/src/SecureClient.cpp')
s=p.read_text();a=s.index('int SecureClient::connect(const char* host');b=s.index('\nint SecureClient::connect(IPAddress',a)
code=r'''
#include <cassert>
#include <cstdint>
#define FREEINK_DEVICE_METALIO_PLUS 1
struct{operator bool(){return false;}void println(const char*){}}Serial;
void*wolfSSLv23_client_method(){return nullptr;}void*wolfTLSv1_2_client_method(){return nullptr;}
namespace freeink{
struct SecureClient{
 bool _aborted=false,_retryTls12=false;int calls=0,scenario=0;
 int connect(const char*,uint16_t);
 int connectWithMethod(const char*,uint16_t,void*,const char*){
  ++calls;
  if(scenario==1){_retryTls12=true;return calls==2;}
  if(scenario==2)_aborted=true;
  return scenario==3;
 }
};
'''+s[a:b]+r'''
}
int main(){
 freeink::SecureClient c;
 // Resolver, socket, allocation and handshake timeout paths never arm retry.
 assert(c.connect("example.com",443)==0 && c.calls==1);
 c.calls=0;c.scenario=1;assert(c.connect("example.com",443)==1 && c.calls==2);
 c.calls=0;c.scenario=2;assert(c.connect("example.com",443)==0 && c.calls==1);
 c.calls=0;c.scenario=3;assert(c.connect("example.com",443)==1 && c.calls==1);
 c.calls=0;c.scenario=0;assert(c.connect("example.com",443)==0 && c.calls==1 && !c._retryTls12);
}
'''
# Only the fatal negotiation branch sets the retry latch.
assert s.count('_retryTls12 = true;')==1
assert s.index('if (!isWantIo(err))') < s.index('_retryTls12 = true;') < s.index('wolfSSL_ERR_error_string')
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Plus TLS retry: no fallback on DNS/TCP/allocation/timeout, fatal TLS version fallback retained, abort and reuse reset passed')
