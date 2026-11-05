"""Compile production avatar validation/import against failure-capable SD seams."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class RickyProfileTest(unittest.TestCase):
    def test_import_and_failures_preserve_previous_avatar(self):
        source = (ROOT / 'src/components/RickyProfile.cpp').read_text()
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
struct Data {std::vector<uint8_t> bytes; bool valid=true; int width=128,height=128,row=16;};
std::map<std::string,Data> files;
bool failWrite=false, failConvert=false, truncateConvert=false, failSave=false;
struct HalFile {
 Data* data=nullptr; size_t pos=0;
 bool isDirectory() const {return false;}
 size_t fileSize() const {return data->bytes.size();}
 size_t position() const {return pos;}
 bool seek(size_t p) {pos=p;return true;}
 int read(uint8_t* out,size_t n) {
   n=std::min(n,fileSize()-std::min(pos,fileSize()));
   memcpy(out,data->bytes.data()+pos,n);pos+=n;return n;
 }
 size_t write(const uint8_t* in,size_t n) {
   if(failWrite)return 0;
   data->bytes.insert(data->bytes.end(),in,in+n);return n;
 }
 void flush() {}
};
struct StorageType {
 bool ensureDirectoryExists(const char*) {return true;}
 bool openFileForRead(const char*,const std::string& p,HalFile& f) {
   auto i=files.find(p);if(i==files.end())return false;f.data=&i->second;return true;
 }
 bool openFileForWrite(const char*,const char* p,HalFile& f) {
   if(failWrite)return false;files[p]=Data{};files[p].bytes.clear();f.data=&files[p];return true;
 }
} Storage;
enum class BmpReaderError {Ok,Failed};
struct Bitmap {
 HalFile& f;explicit Bitmap(HalFile& f):f(f) {}
 BmpReaderError parseHeaders() {f.pos=62;return f.data->valid?BmpReaderError::Ok:BmpReaderError::Failed;}
 int getWidth() const {return f.data->width;}
 int getHeight() const {return f.data->height;}
 int getRowBytes() const {return f.data->row;}
};
struct Settings {
 char rickyAvatarPath[96]="/.crosspoint/ricky-avatar-a.bmp";
 bool saveToFile() {return !failSave;}
} SETTINGS;
namespace FsHelpers {
 bool extension(const std::string& p,const char* ext) {return p.ends_with(ext);}
 bool hasPngExtension(const std::string& p) {return extension(p,".png");}
 bool hasJpgExtension(const std::string& p) {return extension(p,".jpg");}
 bool hasBmpExtension(const std::string& p) {return extension(p,".bmp");}
}
bool convert(HalFile&,HalFile& out,int,int) {
 if(failConvert)return false;
 out.data->bytes.resize(truncateConvert?62:62+128*16);return true;
}
namespace PngToBmpConverter {bool pngFileTo1BitBmpStreamWithSize(HalFile& in,HalFile& out,int w,int h){return convert(in,out,w,h);}}
namespace JpegToBmpConverter {bool jpegFileTo1BitBmpStreamWithSize(HalFile& in,HalFile& out,int w,int h){return convert(in,out,w,h);}}
namespace PanelTileCache {void forget(const char*){}}
#define LOG_ERR(...) ((void)0)
namespace RickyProfile {
''' + method(source, 'bool validAvatarBitmap(') + method(source, 'bool setAvatar(') +
                method(source, 'bool importAvatar(') + r'''
}
int main() {
 const std::string a="/.crosspoint/ricky-avatar-a.bmp", b="/.crosspoint/ricky-avatar-b.bmp";
 files[a].bytes.assign(62+128*16,7);const auto original=files[a].bytes;
 files["/photo.jpg"].bytes.assign(200,3);
 files["/photo.png"].bytes.assign(200,4);
 files["/photo.bmp"].bytes.assign(62+128*16,5);
 for(int failure=0;failure<4;++failure) {
   failWrite=failure==0;failConvert=failure==1;truncateConvert=failure==2;failSave=failure==3;
   assert(!RickyProfile::importAvatar("/photo.jpg"));
   assert(std::string(SETTINGS.rickyAvatarPath)==a && files[a].bytes==original);
 }
 failWrite=failConvert=truncateConvert=failSave=false;
 assert(!RickyProfile::importAvatar(b));
 assert(!RickyProfile::importAvatar("relative.jpg"));
 assert(!RickyProfile::importAvatar("/photo.heic"));
 files["/broken.bmp"].bytes.assign(62,0);
 assert(!RickyProfile::importAvatar("/broken.bmp"));
 assert(RickyProfile::importAvatar("/photo.jpg"));assert(std::string(SETTINGS.rickyAvatarPath)==b);
 assert(files[b].bytes.size()==62+128*16);
 assert(RickyProfile::importAvatar("/photo.png"));assert(std::string(SETTINGS.rickyAvatarPath)==a);
 assert(RickyProfile::importAvatar("/photo.bmp"));assert(files[b].bytes==files["/photo.bmp"].bytes);
}
''')

    def test_profile_is_shared_and_settings_are_additive(self):
        settings = (ROOT / 'src/CrossPointSettings.cpp').read_text()
        for key in ('rickyNickname', 'rickyAvatarPath'):
            self.assertIn(f'doc["{key}"] = {key}', settings)
            self.assertIn(f'doc["{key}"] | ""', settings)
        home = (ROOT / 'src/components/RickyHomeUi.cpp').read_text()
        self.assertIn('RickyProfile::drawAvatar', home)
        self.assertIn('RickyProfile::nickname()', home)
        activity = (ROOT / 'src/activities/settings/RickyProfileActivity.cpp').read_text()
        self.assertIn('utf8SafeTruncateBuffer', activity)
        self.assertIn('if (failed) memcpy(SETTINGS.rickyNickname, previous', activity)
        self.assertNotIn('Storage.remove', activity)

    def test_jpeg_flags_are_mapped_to_denominators(self):
        source = (ROOT / 'lib/JpegToBmpConverter/JpegToBmpConverter.cpp').read_text()
        start = source.index('const int scaleDenominator =')
        expression = source[start:source.index(';', start) + 1]
        for flags in ((1, 2, 4), (2, 4, 8)):
            half, quarter, eighth = flags
            run_cpp(f'''
#include <cassert>
constexpr int JPEG_SCALE_HALF={half},JPEG_SCALE_QUARTER={quarter},JPEG_SCALE_EIGHTH={eighth};
int denominator(int scaleOption) {{{expression}return scaleDenominator;}}
int main() {{assert(denominator(0)==1);assert(denominator(JPEG_SCALE_HALF)==2);
 assert(denominator(JPEG_SCALE_QUARTER)==4);assert(denominator(JPEG_SCALE_EIGHTH)==8);}}
''')


if __name__ == '__main__':
    unittest.main()
