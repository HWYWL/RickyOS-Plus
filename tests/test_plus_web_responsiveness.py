"""Compile the actual HTTP wrapper/portal methods, not a second implementation."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'firmware/rickyos-plus-1.2/upstream/src/network/CrossPointWebServer.cpp').read_text()
activity = (ROOT / 'firmware/rickyos-plus-1.2/upstream/src/activities/network/CrossPointWebServerActivity.cpp').read_text()


def method(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def check(code, flags=()):
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        (root / 'test.cpp').write_text(code)
        subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', *flags,
                        str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
        subprocess.run([str(root / 'test')], check=True)


wrapper = source[source.index('class CrossPointHttpServer final'):source.index('\n};', source.index('class CrossPointHttpServer final')) + 3]
check(r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <limits>
#define FREEINK_DEVICE_METALIO_PLUS 1
unsigned long now = 0;
unsigned long millis(){return now;}
int expired = 0;
#define LOG_INF(...) (++expired)
enum { HC_NONE, HC_WAIT_READ, HC_WAIT_CLOSE };
struct Client {
 bool live = true; int bytes = 0, stops = 0;
 bool connected(){return live;} int available(){return bytes;}
 void stop(){live=false; ++stops;}
};
struct WebServer {
 struct { bool noDelay=false; void setNoDelay(bool value){noDelay=value;} } _server;
 Client _currentClient;
 int _currentStatus=HC_NONE; unsigned long _statusChange=0;
 int * _currentArgs=nullptr, * _postArgs=nullptr;
 int _currentArgCount=0, _postArgsLen=0, polls=0;
 explicit WebServer(uint16_t){} virtual ~WebServer()=default;
 virtual void handleClient(){++polls; if(!_currentClient.live) _currentStatus=HC_NONE;}
};
''' + wrapper + r'''
int main(){
 CrossPointHttpServer server(80);
 assert(server._server.noDelay);
 WebServer* base=&server;
 server._currentStatus=HC_WAIT_READ; server._statusChange=100;
 now=849; base->handleClient(); assert(server._currentClient.stops==0);
 now=850; base->handleClient(); assert(server._currentClient.stops==1 && expired==1);
 assert(server._currentStatus==HC_NONE && server.polls==2);
 // Delayed but nonempty requests and in-progress protocol states are untouched.
 for(int state : {HC_NONE,HC_WAIT_READ,HC_WAIT_CLOSE}) {
  server._currentClient=Client{}; server._currentClient.bytes=1;
  server._currentStatus=state; now=100000; base->handleClient();
  assert(server._currentClient.stops==0);
 }
 server._currentClient=Client{}; server._currentStatus=HC_WAIT_CLOSE;
 base->handleClient(); assert(server._currentClient.stops==0);
 // millis() rollover and fresh sessions do not immediately expire.
 server._currentStatus=HC_WAIT_READ;
 server._statusChange=std::numeric_limits<unsigned long>::max()-99;
 now=100; base->handleClient(); assert(server._currentClient.stops==0);
 now=651; base->handleClient(); assert(server._currentClient.stops==1);
 server._currentClient=Client{}; server._statusChange=now;
 base->handleClient(); assert(server._currentClient.stops==0);
}
''')

check(r'''
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
struct String:std::string {
 using std::string::string;
 String()=default;
 String(const std::string& s):std::string(s){}
 explicit String(uint16_t n):std::string(std::to_string(n)){}
 bool isEmpty() const{return empty();}
 bool startsWith(const char* prefix)const{return rfind(prefix,0)==0;}
};
struct Address{String toString() const{return "192.168.4.1";}};
struct {Address softAPIP(){return {};}} WiFi;
enum {HTTP_GET, HTTP_OPTIONS};
struct WebServer {
 String host="192.168.4.1",path="/"; int verb=HTTP_GET,code=0;
 std::map<std::string,std::string> headers;
 String hostHeader(){return host;} String uri(){return path;}
 int method(){return verb;}
 void sendHeader(const String& key,const String& value,bool=false){headers[key]=value;}
 void send(int value,const char*,const char*){code=value;}
 void send(int value,const char*,const String&){code=value;}
};
int served=0;
bool sendCompressedContent(WebServer* server,const char*,const char*,size_t){++served; server->code=200; return true;}
#define LOG_DBG(...) ((void)0)
namespace web_zh_cn {constexpr char HomePageHtml[]="chinese";}
namespace web_en {constexpr char HomePageHtml[]="english";}
struct Page{const char* data;size_t size;};
Page localizedPage(const char* a,size_t size,const char*,size_t){return {a,size};}
struct CrossPointWebServer {
 bool apMode=true; uint16_t port=80;
 std::unique_ptr<WebServer> server=std::make_unique<WebServer>();
 void handleRoot() const; void handleNotFound() const;
};
''' + method(source, 'static void redirectToHotspot(') + '\n' +
      method(source, 'void CrossPointWebServer::handleRoot()') + '\n' +
      method(source, 'void CrossPointWebServer::handleNotFound()') + r'''
int main(){
 CrossPointWebServer web;
 web.handleRoot(); assert(served==1 && web.server->code==200);
 web.server->host="192.168.4.1:80"; web.handleRoot(); assert(served==2);
 web.server->host="probe.example"; web.handleRoot();
 assert(served==2 && web.server->code==302);
 assert(web.server->headers["Location"]=="http://192.168.4.1/");
 assert(web.server->headers["Cache-Control"]=="no-store");
 web.server->path="/generate_204"; web.handleNotFound(); assert(web.server->code==302);
 web.server->headers.clear(); web.server->path="/api/missing";
 web.handleNotFound(); assert(web.server->code==404 && web.server->headers.empty());
 web.server->verb=HTTP_OPTIONS; web.handleNotFound(); assert(web.server->code==204);
 web.server->verb=HTTP_GET; web.port=8080; web.server->path="/probe";
 web.handleNotFound(); assert(web.server->headers["Location"]=="http://192.168.4.1:8080/");
 web.server->headers.clear(); web.apMode=false; web.handleNotFound();
 assert(web.server->code==404 && web.server->headers.empty());
 web.handleRoot(); assert(served==3 && web.server->code==200);
}
''')

check(r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
constexpr int WIFI_AP=2,AP_CHANNEL=1,AP_MAX_CONNECTIONS=4,DNS_PORT=53;
const char* AP_PASSWORD=nullptr;
const char* AP_SSID="test-hotspot";
const char* AP_HOSTNAME="test-hotspot";
namespace NetworkStartup {bool allowed=true; bool setMode(int,int){return allowed;}}
void delay(int){}
struct IPAddress{int operator[](int)const{return 1;}};
struct{bool allowed=true; int apCalls=0;
 bool softAP(const char*,const char*,int,bool,int){++apCalls;return allowed;}
 IPAddress softAPIP(){return {};}}WiFi;
namespace DNSReplyCode{constexpr int NoError=0;}
bool dnsOk=true;
struct DNSServer{void setErrorReplyCode(int){} bool start(int,const char*,IPAddress){return dnsOk;}};
DNSServer* dnsServer=nullptr;
void stopDnsServer(){delete dnsServer;dnsServer=nullptr;}
void restartMdns(const char*,const char*){}
struct CrossPointWebServerActivity{
 int renderer=0,home=0,web=0; std::string connectedIP,connectedSSID;
 void onGoHome(){++home;} void startWebServer(){++web;}
 void startAccessPoint();
};
''' + method(activity, 'void CrossPointWebServerActivity::startAccessPoint()') + r'''
int main(){
 CrossPointWebServerActivity a;
 NetworkStartup::allowed=false; a.startAccessPoint(); assert(a.home==1 && a.web==0);
 NetworkStartup::allowed=true; WiFi.allowed=false;
 a.startAccessPoint(); assert(a.home==2 && a.web==0);
 WiFi.allowed=true; dnsOk=false;
 a.startAccessPoint(); assert(a.home==2 && a.web==1 && !dnsServer && !a.connectedIP.empty());
 dnsOk=true; a.startAccessPoint(); assert(a.home==2 && a.web==2 && dnsServer);
 stopDnsServer();
}
''')
print('Plus web: empty-connection bound, TCP_NODELAY, partial/upload preservation, rollover, canonical portal, API/STA routes and DNS failure fallback passed (ASan/UBSan)')
