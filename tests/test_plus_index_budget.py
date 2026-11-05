"""Execute production HTTPS request policy against simulated stalled/retried routes."""
from pathlib import Path
import subprocess
import tempfile
source = Path('firmware/rickyos-plus-1.2/upstream/src/network/HttpDownloader.cpp').read_text()
body = source[source.index('constexpr uint32_t HTTP_TIMEOUT_MS'):source.index('\n}  // namespace')]
code = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define CROSSPOINT_VERSION "test"
uint32_t now=0; uint32_t millis(){return now;}
struct HalSystem{static const char* getDeviceModel(){return "test";}};
struct WifiPowerSaveGuard{};
struct HttpDownloader {
 using Header=std::pair<std::string,std::string>;
 using SourceCallback=void (*)(const char*,void*);
 enum DownloadError{OK,HTTP_ERROR,FILE_ERROR,ABORTED,UNAUTHORIZED};
};
namespace freeink {
struct FetchOptions{bool redirectToHttp=false;};
struct FetchSink{};
struct SecureHttpClient {
 uint32_t timeout=0; std::string host="example.com";
 const std::string& getHost() const{return host;}
 void setTimeout(uint32_t value){timeout=value;}
 void setCACert(const char*){} void setInsecure(){} void setUserAgent(const char*){}
 void setBasicAuth(const std::string&, const std::string&){}
 void addHeader(const std::string&,const std::string&){}
};
struct FetchResult {size_t bytes=0,total=0;int status=200;bool complete=true,aborted=false,stopped=false;};
int scenario=0; std::vector<uint32_t> waits;
FetchResult fetchResumable(const std::string&,const FetchOptions&,
 const std::function<void(SecureHttpClient&,bool)>& configure,const FetchSink&,const std::function<bool()>& abort){
 SecureHttpClient http;FetchResult result;
 configure(http,true);waits.push_back(http.timeout);
 if(scenario==4){http.host="release-assets.githubusercontent.com";configure(http,false);result.aborted=abort();return result;}
 if(scenario==0){now+=100;assert(!abort());result.bytes=2252;return result;}
 if(scenario==3){configure(http,false);result.aborted=abort();return result;}
 now+=8000;assert(!abort());
 // TLS 1.2 fallback, redirect or body resume gets only the remaining budget.
 configure(http,true);waits.push_back(http.timeout);
 now+=4000;
 result.aborted=abort();result.complete=!result.aborted;
 return result;
}
}
'''+body+r'''
int main(){
 std::vector<std::string> sources;
 const auto observer=+[](const char* host,void* ctx){static_cast<std::vector<std::string>*>(ctx)->emplace_back(host);};
 freeink::scenario=4;now=0;
 assert(runGetSecure("https://example.com/font","","",{},freeink::FetchSink{},nullptr,nullptr,false,nullptr,60000,nullptr,0,observer,&sources)==HttpDownloader::OK);
 assert((sources==std::vector<std::string>{"example.com","release-assets.githubusercontent.com"}));
 sources.clear();now=0;
 assert(runGetSecure("https://example.com/firmware","","",{},freeink::FetchSink{},nullptr,nullptr,false,"trusted-ca",60000,nullptr,0,observer,&sources)==HttpDownloader::ABORTED);
 assert((sources==std::vector<std::string>{"example.com"}));
 const freeink::FetchSink sink{};
 now=0;freeink::scenario=0;
 assert(runGetSecure("https://example.com/list","","",{},sink,nullptr,nullptr,false,nullptr,12000,nullptr,12000)==HttpDownloader::OK);
 assert(freeink::waits.back()==12000);
 freeink::scenario=1;now=0;freeink::waits.clear();
 assert(runGetSecure("https://example.com/list","","",{},sink,nullptr,nullptr,false,nullptr,12000,nullptr,12000)==HttpDownloader::HTTP_ERROR);
 assert((freeink::waits==std::vector<uint32_t>{12000,4000}));
 // A caller cancellation still takes precedence over an expired route budget.
 bool cancelled=false;now=0;
 const std::function<void()> poll=[&]{if(now==12000)cancelled=true;};
 assert(runGetSecure("https://example.com/list","","",{},sink,&cancelled,nullptr,false,nullptr,12000,poll,12000)==HttpDownloader::ABORTED);
 // Existing large-file downloads keep their long per-operation timeouts.
 now=0;freeink::waits.clear();
 assert(runGetSecure("https://example.com/font","","",{},sink)==HttpDownloader::OK);
 assert((freeink::waits==std::vector<uint32_t>{60000,60000}));
 // Shared-budget subtraction stays correct across the millis rollover.
 now=UINT32_MAX-100;freeink::waits.clear();
 assert(runGetSecure("https://example.com/list","","",{},sink,nullptr,nullptr,false,nullptr,12000,nullptr,12000)==HttpDownloader::HTTP_ERROR);
 assert((freeink::waits==std::vector<uint32_t>{12000,4000}));
 now=0;freeink::scenario=3;
 assert(runGetSecure("https://example.com/firmware","","",{},sink,nullptr,nullptr,false,"trusted-ca",12000,nullptr,12000)==HttpDownloader::ABORTED);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-variable','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Index route budget: successful response, shared retry deadline, cancellation precedence, large file timeout, rollover and verified redirect passed')
