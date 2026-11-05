"""Production JPEG callback differential and render-loop interrupted-idle retry.

These host tests verify pixel identity and scheduling, not physical speed.
"""
from pathlib import Path
import importlib.util
import subprocess
import sys
import tempfile

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT / 'firmware/rickyos-plus-1.2/upstream'
sys.path.insert(0, str(ROOT / 'scripts/tests'))
from test_reading_ui_regressions import method

source = (ROOT / 'lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp').read_text()
callback = method(source, 'int jpegDrawCallback(')
context = method(source, 'struct JpegContext') + ';'
program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>
#include "CancelCheck.h"
#include "SampleRowClamp.h"
enum class DecodeOutput {FrameBufferAndCache,NativeGrayscale16};
struct RenderConfig {
  int x=0,y=0;
  bool useDithering=false,bilinearScaling=true;
  DecodeOutput output=DecodeOutput::NativeGrayscale16;
  CancelCheck cancellation;
};
struct GfxRenderer {
  bool columns=false;
  std::vector<uint8_t> pixels=std::vector<uint8_t>(256*256,0xa5);
  int writes=0,firstX=-1,firstY=-1,secondX=-1,secondY=-1;
  bool grayscale16PrefersColumns()const{return columns;}
  void drawGrayscale16Pixel(int x,int y,uint8_t gray){
    assert(x>=0&&x<256&&y>=0&&y<256);
    if(writes==0){firstX=x;firstY=y;}
    if(writes==1){secondX=x;secondY=y;}
    ++writes;pixels[y*256+x]=gray;
  }
};
struct ImageToFramebufferDecoder {static void yieldDuringDecode(uint32_t&) {}};
struct DirectPixelWriter {void init(GfxRenderer&){assert(false);}void beginRow(int){assert(false);}void writePixel(int,uint8_t){assert(false);}};
struct DirectCacheWriter {void init(void*,int,int,int){assert(false);}void beginRow(int,int){assert(false);}void writePixel(int,uint8_t){assert(false);}};
struct PixelCache {void* buffer=nullptr;int bytesPerRow=0,bandRows=0,originX=0,bandStart=0;bool advanceTo(int){assert(false);return false;}};
uint8_t applyBayerDither4Level(uint8_t,int,int){assert(false);return 0;}
constexpr int FP_SHIFT=16,FP_ONE=1<<16,FP_MASK=FP_ONE-1;
struct JPEGDRAW {void* pUser;void* pPixels;int iWidth,iWidthUsed,iHeight,x,y;};
''' + context + callback + r'''
void draw(GfxRenderer& renderer, int sw,int sh,int dw,int dh,int x,int y,bool filter){
  RenderConfig cfg;cfg.x=x;cfg.y=y;cfg.bilinearScaling=filter;
  JpegContext ctx;ctx.renderer=&renderer;ctx.config=&cfg;ctx.screenWidth=ctx.screenHeight=256;
  ctx.scaledSrcWidth=sw;ctx.scaledSrcHeight=sh;ctx.dstWidth=dw;ctx.dstHeight=dh;
  ctx.fineScaleFPX=(dw<<16)/sw;ctx.fineScaleFPY=(dh<<16)/sh;
  ctx.invScaleFPX=(sw<<16)/dw;ctx.invScaleFPY=(sh<<16)/dh;
  for(int by=0;by<sh;by+=16)for(int bx=0;bx<sw;bx+=16){
    int rows=std::min(16,sh-by),cols=std::min(16,sw-bx);
    std::vector<uint8_t> samples(rows*20,0x11); // deliberately padded source stride
    for(int sy=0;sy<rows;++sy)for(int sx=0;sx<cols;++sx)
      samples[sy*20+sx]=static_cast<uint8_t>((by+sy)*13+(bx+sx)*47);
    JPEGDRAW block{&ctx,samples.data(),20,cols,rows,bx,by};
    assert(jpegDrawCallback(&block)==1);
  }
}
int main(){
  int cases=0;
  for(bool filter:{false,true})for(int dw:{32,60,61,100,123})for(int dh:{29,60,93,100,151})
  for(int offset:{-5,0,3,220}){
    GfxRenderer rows,columns;columns.columns=true;
    draw(rows,100,100,dw,dh,offset,offset,filter);
    draw(columns,100,100,dw,dh,offset,offset,filter);
    assert(rows.pixels==columns.pixels&&rows.writes==columns.writes);++cases;
  }
  GfxRenderer r;r.columns=true;draw(r,100,100,60,60,3,3,true);
  assert(r.firstX==r.secondX&&r.secondY==r.firstY+1);
  RenderConfig cfg;int checks=0;cfg.cancellation={&checks,[](void* p){return ++*static_cast<int*>(p)>2;}};
  JpegContext ctx;ctx.renderer=&r;ctx.config=&cfg;ctx.screenWidth=ctx.screenHeight=256;
  ctx.scaledSrcWidth=ctx.scaledSrcHeight=100;ctx.dstWidth=ctx.dstHeight=60;
  ctx.fineScaleFPX=ctx.fineScaleFPY=(60<<16)/100;ctx.invScaleFPX=ctx.invScaleFPY=(100<<16)/60;
  std::vector<uint8_t> samples(16*16,77);JPEGDRAW block{&ctx,samples.data(),16,16,16,0,0};
  int before=r.writes;assert(jpegDrawCallback(&block)==0&&r.writes>before&&r.writes<before+81);
  assert(cases==200);
}
'''
with tempfile.TemporaryDirectory(prefix='plus-jpeg-column-') as temp:
    work = Path(temp)
    (work / 'check.cpp').write_text(program)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-DFREEINK_DEVICE_METALIO_PLUS=1', '-I'+str(ROOT / 'lib/Memory'),
                    '-I'+str(ROOT / 'lib/Epub/Epub/converters'),
                    str(work / 'check.cpp'), '-o', str(work / 'check')], check=True)
    subprocess.run([str(work / 'check')], check=True)

spec = importlib.util.spec_from_file_location('reader_cache_seams', ROOT / 'scripts/tests/test_reader_page_cache.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
captured = []
module.run_cpp = lambda program, include_dirs=(): captured.append((program, include_dirs))
module.ReaderPageCacheTest().test_render_task_priorities_and_waiter()
program, includes = captured[0]
program = '#define FREEINK_DEVICE_METALIO_PLUS 1\n#include "activities/MainTabMotion.h"\n' + program
program = program.replace('constexpr int eIncrement=0;', '''
constexpr int eIncrement=0;
constexpr uint32_t kGraySettleDelayMs=1500;
bool graySettleAllowed(){return false;}
''')
program = program.replace('unsigned delay=400;', '''
  bool inputDuringIdle=false,disableDuringIdle=false,switchDuringIdle=false,updateDuringIdle=false;
  bool wantsGraySettle()const{return false;}
  void renderIdleWarmup(uint32_t){}
  unsigned delay=400;
''')
program = program.replace('void renderTaskLoop();', '''
  MainTabMotion mainTabMotion_;
  bool renderDirectGray(RenderLock&&){return false;}
  void settleToGray(RenderLock&&,uint32_t){}
  void renderTaskLoop();
''')
program = program.replace('  ++idles;', '''
  ++idles;
  if(inputDuringIdle&&idles==1)manager->cancelIdleRender("touch",true);
  if(disableDuringIdle)delay=0;
  if(switchDuringIdle)manager->pendingAction=ActivityManager::PendingAction::Push;
  if(updateDuringIdle)manager->requestedUpdate=true;
''')
program = program.replace('int main() {', '''int main() {
  {ActivityManager m;m.currentActivity->inputDuringIdle=true;run(m,{1,0,0});
   assert(m.currentActivity->renders==1&&m.currentActivity->idles==2&&notifications==0);
   assert((waits==std::vector<unsigned>{portMAX_DELAY,400,400,portMAX_DELAY}));}
  for(int reason=0;reason<3;++reason){
   ActivityManager m;m.currentActivity->inputDuringIdle=true;
   m.currentActivity->disableDuringIdle=reason==0;
   m.currentActivity->switchDuringIdle=reason==1;
   m.currentActivity->updateDuringIdle=reason==2;
   run(m,{1,0});assert(m.currentActivity->idles==1&&waits.back()==portMAX_DELAY&&notifications==0);
  }
''')
# Keep the actual loop and cancellation methods; only collaborators are shims.
module_spec = importlib.util.spec_from_file_location('reading_ui', ROOT / 'scripts/tests/test_reading_ui_regressions.py')
runner = importlib.util.module_from_spec(module_spec)
module_spec.loader.exec_module(runner)
runner.run_cpp(program, (*includes, ROOT / 'src'))

contents = method((ROOT / 'src/activities/reader/EpubReaderActivity.cpp').read_text(),
                  'void EpubReaderActivity::renderContents(')
# The dropped draw must be Plus-only and only before native begin. A begin/
# commit failure must still redraw BOTH body and chrome before the B/W submit.
assert '#if FREEINK_DEVICE_METALIO_PLUS\n  // beginGrayscale16()' in contents
assert 'if (!use16LevelText) renderStatusBar();\n#else\n  renderStatusBar();\n#endif' in contents
fallback = contents[contents.index('renderer.cancelGrayscale16();\n    renderer.setRenderMode'):]
assert fallback.index('renderPageWithGuideLines();') < fallback.index('renderStatusBar();') < fallback.index('renderer.displayBuffer(HalDisplay::FULL_REFRESH);')
print('200 JPEG sample-equivalence cases, portrait write order, cancellation and production idle retry/waiter policy passed')
