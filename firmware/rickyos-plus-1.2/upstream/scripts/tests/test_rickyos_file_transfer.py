"""Execute production font-upload methods with fault-injectable HAL seams."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class FileTransferTest(unittest.TestCase):
    def test_font_upload_faults_and_chunk_boundaries(self):
        source = (ROOT / 'src/network/CrossPointWebServer.cpp').read_text()
        header = (ROOT / 'src/network/CrossPointWebServer.h').read_text()
        installer = (ROOT / 'src/FontInstaller.cpp').read_text()
        program = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace memory { using ByteBuffer=std::unique_ptr<uint8_t[]>; }
bool bufferAvailable=true;
memory::ByteBuffer makeWebBuffer(size_t size) {
 return bufferAvailable ? std::make_unique<uint8_t[]>(size) : nullptr;
}
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
void resetTaskWatchdogIfSubscribed() {}
struct String : std::string {
 using std::string::string;
 void replace(char a,char b) {std::replace(begin(),end(),a,b);}
};
struct HalFile {
 std::shared_ptr<std::vector<uint8_t>> data;
 static inline size_t writeLimit=std::numeric_limits<size_t>::max();
 size_t write(const uint8_t* p,size_t n) {
   if(!data) return 0;
   n=std::min(n,writeLimit); data->insert(data->end(),p,p+n); return n;
 }
 void close() {data.reset();}
 bool isOpen() const {return bool(data);}
};
struct StorageMock {
 std::map<std::string,std::shared_ptr<std::vector<uint8_t>>> files;
 bool failOpen=false, failPromotion=false, failRollback=false;
 bool exists(const char* p) const {return files.count(p);}
 bool remove(const char* p) {return files.erase(p);}
 bool openFileForWrite(const char*,const char* p,HalFile& f) {
   if(failOpen) return false;
   files[p]=std::make_shared<std::vector<uint8_t>>(); f.data=files[p]; return true;
 }
 bool rename(const char* from,const char* to) {
   std::string path=from;
   if((failPromotion && path.ends_with(".upload-part")) ||
      (failRollback && path.ends_with(".upload-backup"))) return false;
   if(!exists(from) || exists(to)) return false;
   files[to]=files.at(from); files.erase(from); return true;
 }
 void seed(const std::string& p,const std::vector<uint8_t>& bytes) {
   files[p]=std::make_shared<std::vector<uint8_t>>(bytes);
 }
} Storage;
enum HTTPUploadStatus {UPLOAD_FILE_START,UPLOAD_FILE_WRITE,UPLOAD_FILE_END,UPLOAD_FILE_ABORTED};
struct HTTPUpload {
 HTTPUploadStatus status=UPLOAD_FILE_START;
 String filename="Test_18.cpfont";
 size_t totalSize=0,currentSize=0;
 uint8_t* buf=nullptr;
};
struct WebServer {
 HTTPUpload event;
 String family="Test";
 int response=0;
 HTTPUpload& upload() {return event;}
 String arg(const char*) {return family;}
 void send(int code,const char*,const char*) {response=code;}
};
struct SdCardFontRegistry {
 static const char* findFamilyRoot(const char*) {return nullptr;}
 static const char* defaultWriteRoot() {return "/.fonts";}
};
struct FontSystem {
 int dirty=0;
 SdCardFontRegistry& registry() {static SdCardFontRegistry r;return r;}
 void markRegistryDirty() {++dirty;}
} sdFontSystem;
struct FontInstaller {
 FontInstaller(SdCardFontRegistry&) {}
 static bool isValidFamilyName(const char*);
 static bool isValidCpfontFilename(const char*);
 bool ensureFamilyDir(const char*) {return true;}
 static void buildFontPath(const char* f,const char* n,char* p,size_t size) {
   snprintf(p,size,"/.fonts/%s/%s",f,n);
 }
};
''' + method(installer, 'bool FontInstaller::isValidFamilyName(') + '\n' + method(
            installer, 'bool FontInstaller::isValidCpfontFilename(') + r'''
struct CrossPointWebServer {
 std::unique_ptr<WebServer> server=std::make_unique<WebServer>();
''' + method(header, 'struct FontUploadState') + r''' fontUpload;
 CrossPointWebServer() {fontUpload.buffer=std::make_unique<uint8_t[]>(FontUploadState::BUFFER_SIZE);}
 bool flushFontUploadBuffer();
 void abortFontUpload();
 bool commitFontUpload();
 void handleFontUploadData();
 void handleFontUpload();
 bool dropUploadIfCancelled() const { return false; }
};
''' + '\n'.join(method(source, signature) for signature in (
            'bool CrossPointWebServer::flushFontUploadBuffer(',
            'void CrossPointWebServer::abortFontUpload(',
            'bool CrossPointWebServer::commitFontUpload(',
            'void CrossPointWebServer::handleFontUploadData(',
            'void CrossPointWebServer::handleFontUpload(')) + r'''
const std::string finalPath="/.fonts/Test/Test_18.cpfont";
const std::vector<uint8_t> original{'o','l','d'};
std::vector<uint8_t> font(size_t size=9001) {
 std::vector<uint8_t> data(size,0x5a);
 if(size>=8) memcpy(data.data(),"CPFONT\0\0",8);
 return data;
}
void reset() {Storage=StorageMock();HalFile::writeLimit=std::numeric_limits<size_t>::max();sdFontSystem.dirty=0;bufferAvailable=true;}
void start(CrossPointWebServer& s) {s.server->event.status=UPLOAD_FILE_START;s.handleFontUploadData();}
void write(CrossPointWebServer& s,std::vector<uint8_t>& bytes,size_t chunk=4096) {
 for(size_t pos=0;pos<bytes.size();pos+=chunk) {
   s.server->event.status=UPLOAD_FILE_WRITE;
   s.server->event.currentSize=std::min(chunk,bytes.size()-pos);
   s.server->event.buf=bytes.data()+pos;s.handleFontUploadData();
 }
}
void end(CrossPointWebServer& s,size_t size) {
 s.server->event.status=UPLOAD_FILE_END;s.server->event.totalSize=size;
 s.handleFontUploadData();s.handleFontUpload();
}
void oldPreserved() {assert(Storage.exists(finalPath.c_str()));assert(*Storage.files.at(finalPath)==original);}
int main() {
 // Arbitrarily split network prefixes and both buffer-size boundary cases.
 for(size_t chunk : {size_t(1),size_t(3),size_t(7),size_t(4096),size_t(8192)}) {
   reset();CrossPointWebServer s;Storage.seed(finalPath,original);auto bytes=font();
   start(s);oldPreserved();write(s,bytes,chunk);oldPreserved();end(s,bytes.size());
   assert(s.server->response==200 && sdFontSystem.dirty==1);
   assert(*Storage.files.at(finalPath)==bytes && Storage.files.size()==1);
 }
 // Invalid/empty prefixes must not erase an installed font or mark the registry dirty.
 for(size_t size : {size_t(0),size_t(3),size_t(7),size_t(100)}) {
   reset();CrossPointWebServer s;Storage.seed(finalPath,original);std::vector<uint8_t> bad(size,'x');
   start(s);write(s,bad,3);end(s,bad.size());oldPreserved();
   assert(s.server->response==400 && sdFontSystem.dirty==0 && Storage.files.size()==1);
 }
 // SD short writes during a full buffer and during the final flush are reported.
 for(size_t size : {size_t(100),size_t(9001)}) {
   reset();CrossPointWebServer s;Storage.seed(finalPath,original);auto bytes=font(size);
   start(s);HalFile::writeLimit=11;write(s,bytes);end(s,bytes.size());oldPreserved();
   assert(s.server->response==500 && Storage.files.size()==1 && sdFontSystem.dirty==0);
 }
 {reset();CrossPointWebServer s;Storage.seed(finalPath,original);auto bytes=font();
  start(s);write(s,bytes);end(s,bytes.size()+1);oldPreserved();assert(s.server->response==400);}
 {reset();CrossPointWebServer s;Storage.seed(finalPath,original);auto bytes=font();
  start(s);write(s,bytes);s.server->event.status=UPLOAD_FILE_ABORTED;s.handleFontUploadData();
  s.handleFontUpload();oldPreserved();assert(Storage.files.size()==1 && s.server->response==400);}
 {reset();CrossPointWebServer s;Storage.seed(finalPath,original);auto bytes=font();
  start(s);write(s,bytes);Storage.failPromotion=true;end(s,bytes.size());oldPreserved();
  assert(s.server->response==500 && Storage.files.size()==1);}
 {reset();CrossPointWebServer s;Storage.seed(finalPath,original);auto bytes=font();
  start(s);write(s,bytes);Storage.failPromotion=true;Storage.failRollback=true;end(s,bytes.size());
  assert(s.server->response==500 && *Storage.files.at(finalPath+".upload-backup")==original);}
 // Do not overwrite or remove unresolved files left by another/interrupted request.
 for(const char* suffix : {".upload-part",".upload-backup"}) {
   reset();CrossPointWebServer s;Storage.seed(finalPath,original);
   Storage.seed(finalPath+suffix,original);auto bytes=font();start(s);write(s,bytes);end(s,bytes.size());
   oldPreserved();assert(s.server->response==500 && *Storage.files.at(finalPath+suffix)==original);
 }
 {reset();CrossPointWebServer s;auto bytes=font();start(s);write(s,bytes);start(s);
  assert(s.fontUpload.bytesWritten==0 && !s.fontUpload.complete);
  write(s,bytes);end(s,bytes.size());assert(s.server->response==200);}
 {reset();CrossPointWebServer s;Storage.failOpen=true;start(s);end(s,0);
  assert(s.server->response==500 && Storage.files.empty());}
 {reset();CrossPointWebServer s;s.fontUpload.buffer.reset();bufferAvailable=false;start(s);end(s,0);
  assert(s.server->response!=200 && Storage.files.empty());}
 {reset();CrossPointWebServer s;s.server->family=String(120,'A');start(s);end(s,0);
  assert(s.server->response==400 && Storage.files.empty());}
 {reset();CrossPointWebServer s;s.server->event.filename="../../evil.cpfont";start(s);end(s,0);
  assert(s.server->response==400 && Storage.files.empty());}
 // START without END is not a successful upload.
 {reset();CrossPointWebServer s;start(s);s.handleFontUpload();assert(s.server->response!=200);s.abortFontUpload();}
}
'''
        run_cpp(program)

    def test_both_cache_releases_are_locked(self):
        source = (ROOT / 'src/activities/network/CrossPointWebServerActivity.cpp').read_text()
        for signature in ('void CrossPointWebServerActivity::onEnter(',
                          'void CrossPointWebServerActivity::startWebServer('):
            body = method(source, signature)
            self.assertIn('RenderLock lock;', body)
            self.assertLess(body.index('RenderLock lock;'), body.index('fcm->releaseSdFontCaches();'))

    def test_startup_oom_and_partial_cleanup(self):
        source = (ROOT / 'src/network/CrossPointWebServer.cpp').read_text()
        begin = method(source, 'bool CrossPointWebServer::begin(')
        self.assertNotIn('new WebDAVHandler', begin)
        self.assertIn('makeUniqueNoThrow<WebDAVHandler>()', begin)
        self.assertIn('server->addHandler(davHandler.release());', begin)
        for condition in ('if (!davHandler)', 'if (!upload.buffer || !fontUpload.buffer)'):
            failure = method(begin, condition)
            self.assertIn('LOG_ERR(', failure)
            self.assertIn('stop();', failure)
            self.assertIn('return false;', failure)
        stop = method(source, 'void CrossPointWebServer::stop(')
        self.assertNotIn('if (!running || !server)', stop)
        self.assertIn('abortFontUpload();', stop)
        self.assertIn('upload.buffer.reset();', stop)
        self.assertIn('fontUpload.buffer.reset();', stop)


if __name__ == '__main__':
    unittest.main()
