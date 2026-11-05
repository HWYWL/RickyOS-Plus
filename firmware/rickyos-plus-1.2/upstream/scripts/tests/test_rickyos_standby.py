"""Exercise product boot and retained-page drawing; guard wallpaper navigation."""
from pathlib import Path
import unittest
from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class RickyStandbyTest(unittest.TestCase):
    def test_static_boot_product_and_stock(self):
        splash = method((ROOT / 'src/activities/boot_sleep/BootActivity.cpp').read_text(),
                        'void BootActivity::renderSplash(')
        program = r'''
#include <cassert>
#include <cstring>
constexpr const char* CROSSPOINT_VERSION="test-version";
struct HalDisplay {enum {FULL_REFRESH=1};};
struct Renderer {int paints=0,mode=0;void displayBuffer(int m=0){++paints;mode=m;}};
struct Theme {
 int staticMarks=0,stockMarks=0;
 void drawRickyPowerScreen(Renderer&,bool asleep,bool transition){assert(!asleep&&!transition);++staticMarks;}
 void drawSplash(Renderer&,const char*,const char* version){assert(!strcmp(version,CROSSPOINT_VERSION));++stockMarks;}
} GUI;
struct BootActivity {Renderer renderer;void renderSplash();};
''' + splash + r'''
int main(){BootActivity boot;boot.renderSplash();assert(boot.renderer.paints==1);
#ifdef RICKYOS_PRODUCT
 assert(GUI.staticMarks==1&&GUI.stockMarks==0&&boot.renderer.mode==HalDisplay::FULL_REFRESH);
#else
 assert(GUI.staticMarks==0&&GUI.stockMarks==1);
#endif
}
'''
        run_cpp(program, defines=('RICKYOS_PRODUCT',))
        run_cpp(program)

    def test_badge_only_changes_its_small_safe_rectangle(self):
        badge = method((ROOT / 'src/components/themes/BaseTheme.cpp').read_text(),
                       'Rect BaseTheme::drawRickyStandbyIndicator(')
        program = r'''
#include <algorithm>
#include <cassert>
#include <vector>
#include <cstring>
#include "components/Rect.h"
constexpr int UI_10_FONT_ID=0,STR_RICKY_POWERED_OFF=1;
namespace RickyDesignUi {constexpr int CAPTION=7;}
enum class Color {White};
const char* tr(int id){return id?"Off":"Standby";}
struct GfxRenderer {
 Rect safe;int screenWidth,screenHeight;
 mutable std::vector<int> pixels;
 mutable Rect badge{};
 mutable int ink=0,drawnInk=-1;
 struct ClipScope{ClipScope(const GfxRenderer&,int,int,int,int){}};
 struct TextInkScope{
   const GfxRenderer& r;int previous;
   TextInkScope(const GfxRenderer& renderer,int level):r(renderer),previous(r.ink){r.ink=level;}
   ~TextInkScope(){r.ink=previous;}
 };
 int getTextWidth(int font,const char* text)const{
   assert(font==RickyDesignUi::CAPTION);return !strcmp(text,"Standby")?70:140;
 }
 int getLineHeight(int)const{return 28;}
 void fillRoundedRect(int x,int y,int w,int h,int radius,Color color)const{
   assert(radius==std::min(w,h)/2&&color==Color::White);
   badge=Rect{x,y,w,h};
   for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx){
     assert(xx>=safe.x&&yy>=safe.y&&xx<safe.x+safe.width&&yy<safe.y+safe.height);
     pixels[yy*screenWidth+xx]=0;
   }
 }
};
struct UITheme {
 static UITheme& getInstance(){static UITheme value;return value;}
 Rect getScreenSafeArea(const GfxRenderer& renderer){return renderer.safe;}
 static void drawCenteredWrappedText(const GfxRenderer& r,Rect rect,int font,const char* label,int lines){
   assert((!strcmp(label,"Standby")||!strcmp(label,"Off"))&&lines==1&&rect.x==r.badge.x&&rect.y==r.badge.y);
   assert(font==RickyDesignUi::CAPTION);r.drawnInk=r.ink;
 }
};
struct BaseTheme{static Rect drawRickyStandbyIndicator(const GfxRenderer&,bool=false);};
''' + badge + r'''
int main(){
 for(Rect safe : {Rect{32,32,620,1152},Rect{32,32,1152,620},Rect{12,20,456,760},Rect{20,12,760,456}}){
   GfxRenderer renderer{safe,safe.x+safe.width+32,safe.y+safe.height+32,{}, Rect{}};
   renderer.pixels.assign(renderer.screenWidth*renderer.screenHeight,7);
   const Rect noStatus=BaseTheme::drawRickyStandbyIndicator(renderer);
   assert(noStatus.width==0&&noStatus.height==0&&renderer.drawnInk==-1);
   assert(std::all_of(renderer.pixels.begin(),renderer.pixels.end(),[](int p){return p==7;}));
   BaseTheme::drawRickyStandbyIndicator(renderer,true);
   assert(renderer.drawnInk==ricky::ds::gray::kInk2&&renderer.ink==0);
   assert(renderer.badge.height==36&&renderer.badge.width==std::max(safe.width/5,172));
   assert(renderer.badge.x+renderer.badge.width<safe.x+safe.width);
   assert(renderer.badge.y+renderer.badge.height<safe.y+safe.height);
   assert(renderer.badge.width*renderer.badge.height < safe.width*safe.height/20);
   for(int y=0;y<renderer.screenHeight;++y)for(int x=0;x<renderer.screenWidth;++x){
     Rect b=renderer.badge;
     bool inside=x>=b.x&&y>=b.y&&x<b.x+b.width&&y<b.y+b.height;
     assert(renderer.pixels[y*renderer.screenWidth+x]==(inside?0:7));
   }
 }
 for(Rect safe:{Rect{0,0,0,100},Rect{0,0,100,0},Rect{0,0,24,20}}){
   GfxRenderer renderer{safe,100,100,{},Rect{}};
   renderer.pixels.assign(10000,7);
   const Rect result=BaseTheme::drawRickyStandbyIndicator(renderer,true);
   assert(result.width<=safe.width&&result.height<=safe.height);
 }
}
'''
        run_cpp(program, include_dirs=(ROOT / 'src',))
        run_cpp(program, include_dirs=(ROOT / 'src',), defines=('CROSSMUX_UI_PROFILE_HIGH_DPI',))

    def test_native_power_off_badge_does_not_flatten_gray_or_square_corners(self):
        body = method((ROOT / 'src/activities/boot_sleep/SleepActivity.cpp').read_text(),
                      'void SleepActivity::renderBitmapSleepScreen(')
        native = body[body.index('if (nativeCustomImage'):body.index('if (!renderer.drawBitmap(')]
        self.assertIn('GUI.drawRickyStandbyIndicator(renderer, true)', native)
        self.assertNotIn('copyBwToGrayscale16', native)
        translations = (ROOT / 'lib/I18n/translations/chinese.yaml').read_text()
        self.assertIn('STR_RICKY_STANDBY: "待机"', translations)

    def test_retained_reader_orientation_restored_without_clear(self):
        draw = method((ROOT / 'src/activities/boot_sleep/SleepActivity.cpp').read_text(),
                      'void SleepActivity::renderLastScreenSleepScreen(')
        program = r'''
#include <cassert>
#include <initializer_list>
struct HalDisplay{enum {FAST_REFRESH=1};};
struct Rect{int x,y,width,height;};
struct GfxRenderer{
 int orientation=0;
 mutable int paints=0,gray=0;
 int getOrientation()const{return orientation;}
 void setOrientation(int o){orientation=o;}
 void displayBuffer(int mode)const{assert(mode==1);++paints;}
 void displayGrayscaleBase(int mode)const{assert(mode==1);++gray;}
 int getScreenWidth()const{return 200;}int getScreenHeight()const{return 100;}
 void getOrientedViewableTRBL(int* t,int* r,int* b,int* l){*t=*r=*b=*l=2;}
};
struct State{bool lastSleepFromReader=true;} APP_STATE;
struct Settings{int orientation=0;} SETTINGS;
struct ReaderUtils{static void applyOrientation(GfxRenderer& r,int o){r.setOrientation(o);}};
struct Theme{int calls=0,lastOrientation=-1;void drawRickyStandbyIndicator(GfxRenderer& r,bool off){assert(off);++calls;lastOrientation=r.orientation;}} GUI;
bool native=false;
struct KeepPageFace{static bool drawRetained(GfxRenderer& r,const Rect& v,bool sticker,bool off){
 assert(!sticker && off && v.width==196 && v.height==96);
 if(native)GUI.drawRickyStandbyIndicator(r,off);return native;}};
struct Gpio{bool x3=false;bool deviceIsX3()const{return x3;}} gpio;
struct SleepActivity{GfxRenderer& renderer;void renderLastScreenSleepScreen()const;};
''' + draw + r'''
int main(){
 for(bool retained:{false,true})for(bool reader:{false,true})for(int orientation:{0,1,2,3})for(bool x3:{false,true}){
   native=retained;
   APP_STATE.lastSleepFromReader=reader;SETTINGS.orientation=orientation;gpio.x3=x3;
   GfxRenderer renderer;SleepActivity sleep{renderer};sleep.renderLastScreenSleepScreen();
   assert(GUI.lastOrientation==(reader?orientation:0));assert(renderer.orientation==0);
   assert(renderer.paints==(!retained&&!x3?1:0)&&renderer.gray==(!retained&&x3?1:0));
 }
}
'''
        run_cpp(program, defines=('RICKYOS_PRODUCT',))

    def test_wallpaper_picker_preview_cancel_and_decode_guard(self):
        # Picking lives on the Apps → Standby & power off page, its one entry.
        settings = (ROOT / 'src/activities/settings/SettingsActivity.cpp').read_text()
        self.assertNotIn('RickyStandbySettingsActivity', settings)
        page = (ROOT / 'src/activities/apps/standby/RickyStandbySettingsActivity.cpp').read_text()
        self.assertIn('FileBrowserActivity::Mode::PickWallpaper', page)
        self.assertIn('startActivityForResultWith<ImageViewerActivity>(previewDone, entry->path, true, sticker)', page)
        self.assertIn('FileBrowserActivity::Mode::PickPng', page)
        viewer = (ROOT / 'src/activities/util/ImageViewerActivity.cpp').read_text()
        on_enter = method(viewer, 'void ImageViewerActivity::onEnter(')
        self.assertIn('imageReady = false', on_enter)
        preview = method(viewer, 'bool ImageViewerActivity::preparePreview(')
        self.assertIn('preparePreview()', on_enter)
        self.assertIn('FrameBufferLoan loan(renderer)', preview)
        self.assertIn('JpegToBmpConverter::Output::Gray8', preview)
        self.assertIn('constexpr bool absolute = false', on_enter)
        self.assertIn('if (!wallpaperPicker && siblingImages.empty()', on_enter)
        self.assertIn('if (wallpaperPicker)', method(viewer, 'void ImageViewerActivity::loop('))
        options = method(viewer, 'void ImageViewerActivity::showSleepCoverOptions(')
        self.assertLess(options.index('if (!imageReady) return'), options.index('doSetSleepCover('))
        self.assertIn('FsHelpers::hasJpgExtension(filePath) ? previewPath.c_str()', options)
        install = method(viewer, 'bool ImageViewerActivity::doSetSleepCover(')
        self.assertIn('copied == expected', install)
        self.assertIn('if (!SETTINGS.saveToFile())', install)
        self.assertIn('SETTINGS.sleepScreen = previousMode', install)
        self.assertIn('Storage.rename(SLEEP_IMAGE_BACKUP_PATH, SLEEP_IMAGE_PATH)', install)

    def test_sleep_network_toasts_do_not_damage_retained_page(self):
        # No plugin delivery on the way into sleep any more: nothing joins Wi-Fi or
        # draws a toast over the retained page; activity state is still saved first.
        source = (ROOT / 'src/main.cpp').read_text()
        self.assertNotIn('pluginevents', source)
        enter = method(source, 'void enterDeepSleep(')
        self.assertIn('activityManager.prepareForSleep();', enter)


if __name__ == '__main__':
    unittest.main()
