"""Production reader gating/AA policy and content-signature decoder selection."""
from pathlib import Path
import sys

PORT=Path(__file__).resolve().parents[1]
ROOT=PORT/'firmware/rickyos-plus-1.2/upstream'
sys.path.insert(0,str(ROOT/'scripts/tests'))
from test_reading_ui_regressions import method, run_cpp

reader=(ROOT/'src/activities/reader/EpubReaderActivity.cpp').read_text()
factory=(ROOT/'lib/Epub/Epub/converters/ImageDecoderFactory.cpp').read_text()
program=r'''
#define FREEINK_DEVICE_METALIO_PLUS 1
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#define LOG_ERR(...) ((void)0)
struct {bool textAntiAliasing=true;uint8_t nativePageTurn=0;} SETTINGS;
namespace ReadingBackground {bool on=false;bool active(){return on;}}
struct Page {bool image=false,panel=false;bool hasImages()const{return image;}
bool hasPanelFill()const{return panel;}};
struct Renderer {bool inverted=false;int levels=16;
bool isInverted()const{return inverted;}int getGrayscaleLevels()const{return levels;}};
struct EpubReaderActivity {Renderer renderer;bool nativeGray16Page(const Page&)const;};
''' + method(reader,'bool EpubReaderActivity::nativeGray16Page(') + method(reader,'const uint8_t* readerTextWeightCurve(') + r'''
bool oom=false;
template<class T> auto makeUniqueNoThrow(){return std::unique_ptr<T>(oom?nullptr:new T);}
struct HalFile {std::vector<uint8_t> bytes;int read(uint8_t* p,size_t n){n=std::min(n,bytes.size());std::copy_n(bytes.data(),n,p);return static_cast<int>(n);}};
struct Store {std::vector<uint8_t> bytes;bool exists=true;
bool openFileForRead(const char*,const std::string&,HalFile& f){f.bytes=bytes;return exists;}} Storage;
struct ImageToFramebufferDecoder {virtual ~ImageToFramebufferDecoder()=default;};
struct JpegToFramebufferConverter:ImageToFramebufferDecoder {static bool supportsFormat(const std::string& s){return s==".jpg"||s==".jpeg";}};
struct PngToFramebufferConverter:ImageToFramebufferDecoder {static bool supportsFormat(const std::string& s){return s==".png";}};
struct ImageDecoderFactory {
static std::unique_ptr<JpegToFramebufferConverter> jpegDecoder;
static std::unique_ptr<PngToFramebufferConverter> pngDecoder;
static ImageToFramebufferDecoder* getDecoder(const std::string&);
};
std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder;
''' + method(factory,'ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(') + r'''
int main(){
EpubReaderActivity r;Page text,image,panel;image.image=true;panel.panel=true;
for(int style=0;style<=4;style++)for(bool aa:{false,true}){
SETTINGS.textAntiAliasing=aa;
SETTINGS.nativePageTurn=style;
assert(r.nativeGray16Page(image));assert(r.nativeGray16Page(panel));
assert(r.nativeGray16Page(text)==(aa || (style>=1 && style<=3)));
for(unsigned weight=0;weight<5;++weight){auto* curve=readerTextWeightCurve(weight);
if(!aa){assert(curve);for(int i=0;i<16;++i)assert(curve[i]==0||curve[i]==15);assert(curve[0]==0&&curve[15]==15);}}
r.renderer.inverted=true;assert(!r.nativeGray16Page(image)&&!r.nativeGray16Page(text)&&!r.nativeGray16Page(panel));r.renderer.inverted=false;
r.renderer.levels=4;assert(!r.nativeGray16Page(image)&&!r.nativeGray16Page(panel));r.renderer.levels=16;
// 阅读背景 lives in the 16-level frame: a plain text page carries it too, never in Night mode.
ReadingBackground::on=true;assert(r.nativeGray16Page(text));
r.renderer.inverted=true;assert(!r.nativeGray16Page(text));r.renderer.inverted=false;ReadingBackground::on=false;
}
Storage.bytes={137,80,78,71,13,10,26,10};
assert(dynamic_cast<PngToFramebufferConverter*>(ImageDecoderFactory::getDecoder("legacy.jpg")));
Storage.bytes={255,216,255,224};
assert(dynamic_cast<JpegToFramebufferConverter*>(ImageDecoderFactory::getDecoder("legacy.png")));
Storage.bytes={1,2,3};assert(!ImageDecoderFactory::getDecoder("unknown.webp"));
Storage.exists=false;assert(dynamic_cast<PngToFramebufferConverter*>(ImageDecoderFactory::getDecoder("lazy.PNG")));
ImageDecoderFactory::jpegDecoder.reset();oom=true;assert(!ImageDecoderFactory::getDecoder("lazy.jpg"));
}
'''
run_cpp(program)

# Validate the production transaction order, not a mock's rewritten order:
# native selection has no initial BW draw or display, and commit terminates it.
contents=method(reader,'void EpubReaderActivity::renderContents(')
assert 'const bool use16LevelText = nativeGray16Page(*page);' in contents
assert 'else if (pageHasImages)' in contents  # Legacy/night-mode fallback remains.
base=contents[contents.index('if (use16LevelText) {'):contents.index('} else if (pageCacheHit)')]
assert 'displayBuffer(' not in base and 'renderPageWithGuideLines();' not in base
native=contents[contents.index('if (use16LevelText) {',contents.index('const auto tDisplay')):contents.index('} else if (tiledGrayscale)')]
assert native.count('renderer.commitGrayscale16()')==1
assert 'renderer.displayGrayBuffer()' not in native
assert 'renderer.cancelGrayscale16()' in native and 'renderer.displayBuffer(HalDisplay::FULL_REFRESH)' in native

# Execute the actual text-finish arming block. The option is a pure-text AA
# hint, not tied to motion, images, CSS panels, or scheduled/manual cleanup.
arming=native[native.index('if (refresh == HalDisplay::FAST_REFRESH && !panelTurn)'):]
arming=arming.split('#endif',1)[0]
run_cpp(r'''
#include <cassert>
#include <initializer_list>
struct HalDisplay {enum {FAST_REFRESH=0,FULL_REFRESH=1};};
struct {bool textAntiAliasing=false;int nativePageTurn=0;} SETTINGS;
struct Renderer {int getScreenWidth(){return 684;}int getScreenHeight(){return 1216;}
void setNextGray16ReaderRefresh(int,int,int,int){}} renderer;
struct {bool called=false,value=false;void setNextGray16ReaderTextFinish(bool v){called=true;value=v;}} display;
int main(){
const int orientedMarginLeft=24,orientedMarginTop=32,orientedMarginRight=24,orientedMarginBottom=64;
for(int refresh:{0,1})for(bool panelTurn:{false,true})for(bool pageHasImages:{false,true})
for(bool readerBackground:{false,true})for(bool aa:{false,true})for(int motion:{0,1,2,3}){
 SETTINGS.textAntiAliasing=aa;SETTINGS.nativePageTurn=motion;display.called=display.value=false;
''' + arming + r'''
 assert(SETTINGS.textAntiAliasing==aa);
 assert(display.called==(refresh==HalDisplay::FAST_REFRESH&&!panelTurn));
 assert(display.value==(refresh==HalDisplay::FAST_REFRESH&&!panelTurn&&!pageHasImages&&!readerBackground));
}}
''')

hal=(ROOT/'lib/hal/HalDisplay.cpp').read_text()
forward=method(hal,'void HalDisplay::setNextGray16ReaderTextFinish(')
for plus in (0,1):
    run_cpp(f'#define FREEINK_DEVICE_METALIO_PLUS {plus}\n'+r'''
#include <cassert>
namespace freeink {bool called=false,value=false;void metalioPlusSetGray16ReaderTextFinish(bool v){called=true;value=v;}}
struct HalDisplay {void setNextGray16ReaderTextFinish(bool);};
'''+forward+r'''
int main(){HalDisplay h;h.setNextGray16ReaderTextFinish(true);
assert(freeink::called==bool(FREEINK_DEVICE_METALIO_PLUS));
if(FREEINK_DEVICE_METALIO_PLUS){assert(freeink::value);h.setNextGray16ReaderTextFinish(false);assert(!freeink::value);}}
''')
print('Plus mixed-image native gate, binary text AA-off coverage, night fallback, signature probing/OOM and single native commit order passed')
