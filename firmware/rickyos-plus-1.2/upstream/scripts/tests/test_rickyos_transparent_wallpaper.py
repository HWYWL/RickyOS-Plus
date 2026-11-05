"""Execute production overlay/install control flow; real PNG pixels are in NativeGrayscaleTest."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp


class TransparentWallpaperTest(unittest.TestCase):
    def test_overlay_rollback_once_and_fit(self):
        source = (ROOT / 'src/activities/apps/standby/KeepPageFace.cpp').read_text()
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <string>
#define LOG_ERR(...) ((void)0)
struct Rect {int x,y,width,height;};
struct GfxRenderer {
  bool available=true,active=false,success=true;
  int begins=0,commits=0,cancels=0,fast=0,pixels=6,visible=6;
  bool beginRetainedGrayscale16(){++begins;if(!available)return false;active=true;pixels=visible;return true;}
  bool commitGrayscale16(){++commits;active=false;if(success)visible=pixels;return success;}
  void cancelGrayscale16(){++cancels;active=false;}
  void displayBuffer(int){++fast;}
  struct ClipScope {ClipScope(GfxRenderer&,int,int,int,int){}};
};
struct HalDisplay {enum {FAST_REFRESH};};
struct {bool present=true;bool exists(const char*)const{return present;}} Storage;
struct CrossPointSettings {enum {FIT,CROP};};
struct {int sleepScreenCoverMode=0;} SETTINGS;
enum class DecodeOutput {NativeGrayscale16};
struct RenderConfig {int maxWidth=0,maxHeight=0,x=0,y=0;float sourceCropX=0,sourceCropY=0;
  bool useExactDimensions=false,preserveAlpha=false,useDithering=true;DecodeOutput output{};};
struct ImageDimensions {int width=0,height=0;};
int decodes=0;bool valid=true,decoded=true;ImageDimensions dimensions{200,100};RenderConfig last;
struct PngToFramebufferConverter {
  bool getDimensions(const std::string&,ImageDimensions& d){d=dimensions;return valid;}
  bool decodeToFramebuffer(const std::string&,GfxRenderer& r,const RenderConfig& cfg){
    ++decodes;last=cfg;r.pixels=9;return decoded;}
};
struct {int badges=0;void drawRickyStandbyIndicator(GfxRenderer&,bool=false){++badges;}} GUI;
struct KeepPageFace {
  static constexpr const char* kStickerPath="/standby.png";
  bool sticker_=true,drawn_=false;
  static bool drawRetained(GfxRenderer&,const Rect&,bool,bool=false);
  bool renderNative(GfxRenderer&,const Rect&);
};
''' + method(source, 'bool KeepPageFace::drawRetained(') + '\n' +
                method(source, 'bool KeepPageFace::renderNative(') + r'''
int main(){
  GfxRenderer r;KeepPageFace face;const Rect view{10,20,100,200};
  assert(face.renderNative(r,view) && r.visible==9 && r.commits==1 && decodes==1);
  assert(last.x==10 && last.y==95 && last.maxWidth==100 && last.maxHeight==50);
  assert(last.preserveAlpha && !last.useDithering && last.useExactDimensions);
  for(int i=0;i<10;++i)assert(face.renderNative(r,view));
  assert(r.commits==1 && decodes==1 && GUI.badges==0); // no repeated alpha or standby label
  SETTINGS.sleepScreenCoverMode=CrossPointSettings::CROP;
  assert(KeepPageFace::drawRetained(r,view,true));
  assert(last.sourceCropX==.75f && last.maxWidth==100 && last.maxHeight==200);
  decoded=false;r.visible=6;int count=r.begins;
  assert(KeepPageFace::drawRetained(r,view,true));
  assert(r.visible==6 && r.begins==count+2); // partial rows discarded before badge
  valid=false;count=decodes;
  assert(KeepPageFace::drawRetained(r,view,true) && decodes==count);
  Storage.present=false;count=r.begins;
  assert(KeepPageFace::drawRetained(r,view,true) && r.begins==count+1);
  r.available=false;KeepPageFace fallback;
  assert(fallback.renderNative(r,view) && r.fast==1 && GUI.badges==0);
  assert(fallback.renderNative(r,view) && r.fast==1);
  r.available=true;
  assert(KeepPageFace::drawRetained(r,view,false,true) && GUI.badges==1);
  r.available=true;r.success=false;
  assert(!KeepPageFace::drawRetained(r,view,false) && !r.active);
  assert(!KeepPageFace::drawRetained(r,{0,0,0,100},true));
}
''')

    def test_install_copy_rename_and_settings_faults(self):
        source = (ROOT / 'src/activities/util/ImageViewerActivity.cpp').read_text()
        face = (ROOT / 'src/activities/apps/standby/KeepPageFace.cpp').read_text()
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <map>
#include <string>
#define LOG_ERR(...) ((void)0)
std::map<std::string,std::string> files;
bool failRead=false,failWrite=false;int failRename=0,renames=0;
struct HalFile {
 std::string path;size_t pos=0;
 uint64_t fileSize64(){return files[path].size();}
 int read(uint8_t* p,size_t n){if(failRead)return -1;n=std::min(n,files[path].size()-pos);
   memcpy(p,files[path].data()+pos,n);pos+=n;return n;}
 size_t write(uint8_t* p,size_t n){if(failWrite)return 0;files[path].append((char*)p,n);return n;}
 void flush(){}
};
struct {
 bool exists(const char* p){return files.count(p);}
 bool remove(const char* p){return files.erase(p);}
 bool rename(const char* a,const char* b){if(++renames==failRename || !exists(a) || exists(b))return false;
   files[b]=files[a];files.erase(a);return true;}
 bool openFileForRead(const char*,const std::string& p,HalFile& f){f.path=p;return files.count(p);}
 bool openFileForWrite(const char*,const char* p,HalFile& f){f.path=p;files[p]="";return true;}
} Storage;
struct CrossPointSettings {enum {RICKY_STANDBY_TRANSPARENT=4};};
struct {uint8_t rickyStandbyFace=3;int failSave=0;
 bool saveToFile(){if(failSave){--failSave;return false;}return true;}} SETTINGS;
struct KeepPageFace {
 static constexpr const char* kStickerPath="/standby.png";
 static constexpr const char* kStickerPartPath="/standby.png.part";
 static constexpr const char* kStickerBackupPath="/standby.png.bak";
 static void recoverSticker();
};
struct ImageViewerActivity {bool imageReady=true,png=true;std::string filePath="/images/new.png";int renderer=0;
 bool isPng(){return png;}bool doSetSticker();};
enum {STR_DONE,STR_FAILED_LOWER};int tr(int v){return v;}
struct {void drawPopup(int,int){}} GUI;void delay(int){}
''' + method(source, 'bool ImageViewerActivity::doSetSticker()') + '\n' +
                method(face, 'void KeepPageFace::recoverSticker()') + r'''
void reset(){files={{"/images/new.png",std::string(4097,'N')},{"/standby.png","old"}};
 failRead=failWrite=false;renames=failRename=0;SETTINGS.rickyStandbyFace=3;SETTINGS.failSave=0;}
int main(){
 ImageViewerActivity viewer;reset();assert(viewer.doSetSticker());
 assert(files["/standby.png"]==files["/images/new.png"] && SETTINGS.rickyStandbyFace==4);
 assert(!files.count("/standby.png.bak") && !files.count("/standby.png.part"));
 for(int fault=0;fault<5;++fault){reset();
   if(fault==0)failRead=true;if(fault==1)failWrite=true;
   if(fault==2)failRename=1;if(fault==3)failRename=2;if(fault==4)SETTINGS.failSave=1;
   assert(!viewer.doSetSticker());assert(files["/standby.png"]=="old" && SETTINGS.rickyStandbyFace==3);
   assert(!files.count("/standby.png.part"));}
 reset();files.erase("/standby.png");SETTINGS.failSave=1;assert(!viewer.doSetSticker());
 assert(!files.count("/standby.png") && SETTINGS.rickyStandbyFace==3);
 reset();files["/standby.png.bak"]="previous";files.erase("/standby.png");
 KeepPageFace::recoverSticker();assert(files["/standby.png"]=="previous");
 reset();files["/standby.png.bak"]="previous";KeepPageFace::recoverSticker();
 assert(files["/standby.png"]=="old"); // never replace a valid installed image
 reset();viewer.png=false;assert(!viewer.doSetSticker() && files["/standby.png"]=="old");
}
''')

    def test_simulator_patch_is_idempotent_and_checked(self):
        spec = importlib.util.spec_from_file_location('sim_patch', ROOT / 'scripts/patch_rickyos_simulator.py')
        patch = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(patch)
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / 'src'
            src.mkdir()
            header, driver = src / 'HalDisplay.h', src / 'HalDisplay.cpp'
            header.write_text('  uint8_t* beginGrayscale16();\n')
            driver.write_text('uint8_t* HalDisplay::beginGrayscale16() {\n}\n')
            patch.patch_retained_gray(src.parent)
            before = (header.read_text(), driver.read_text())
            patch.patch_retained_gray(src.parent)
            self.assertEqual(before, (header.read_text(), driver.read_text()))
            driver.write_text(driver.read_text().replace('const uint8_t gray', 'const uint8_t changed'))
            with self.assertRaises(RuntimeError):
                patch.patch_retained_gray(src.parent)


if __name__ == '__main__':
    unittest.main()
