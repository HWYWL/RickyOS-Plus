"""Execute actual renderer region mapping in all four screen orientations."""
from pathlib import Path
import subprocess
import tempfile
PORT = Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2'
s = (PORT / 'upstream/lib/GfxRenderer/GfxRenderer.cpp').read_text()
start = s.index('namespace {\ntemplate <typename Display>\nvoid configureGrayUi(')
end = s.index('\nbool GfxRenderer::beginGrayscale16()', start)
actual = s[start:end]
program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <array>
struct Display {
 std::array<int,4> body{},nav{},motion{};
 bool adaptive=false;
 void setNextGray16ReaderRefresh(int x,int y,int w,int h){body={x,y,w,h};}
 void setNextGray16UiRefresh(int x,int y,int w,int h,uint8_t,uint8_t,bool a){nav={x,y,w,h};adaptive=a;}
 void setNextMainTabTurn(uint8_t,bool,int x,int y,int w,int h){motion={x,y,w,h};}
};
static Display display;
struct GfxRenderer {
 enum Orientation{Portrait,PortraitInverted,LandscapeClockwise,LandscapeCounterClockwise};
 Orientation orientation;int panelWidth=1216,panelHeight=684;
 int getScreenWidth()const{return orientation==Portrait||orientation==PortraitInverted?panelHeight:panelWidth;}
 int getScreenHeight()const{return orientation==Portrait||orientation==PortraitInverted?panelWidth:panelHeight;}
 void mapGrayRefreshArea(int&,int&,int&,int&)const;
 void setNextGray16UiRefresh(int,int,int,int,uint8_t,uint8_t,bool adaptive=false)const;
 void setNextMainTabTurn(int,int,int,int,bool)const;
 void setNextGray16ReaderRefresh(int,int,int,int)const;
};
''' + actual + r'''
int main(){
 using R=GfxRenderer;
 for(auto o:{R::Portrait,R::PortraitInverted,R::LandscapeClockwise,R::LandscapeCounterClockwise}){
  R r{o};int left=24,top=100,right=r.getScreenWidth()-24,bottom=r.getScreenHeight()-80;
  r.setNextGray16ReaderRefresh(left,top,right-left,bottom-top);
  r.setNextGray16UiRefresh(left,top,right-left,bottom-top,8,32);
  assert(display.body==display.nav && !display.adaptive);
  r.setNextMainTabTurn(left,top,right-left,bottom-top,true);
  assert(display.motion==display.body);
  r.setNextGray16UiRefresh(left,top,right-left,bottom-top,8,32,true);
  assert(display.adaptive);
  int x0=1216,y0=684,x1=0,y1=0;
  // Independent map of every logical point, including the footer's near edge.
  for(int y=0;y<r.getScreenHeight();++y)for(int x=0;x<r.getScreenWidth();++x){
   int px,py;
   if(o==R::Portrait){px=y;py=683-x;}
   else if(o==R::PortraitInverted){px=1215-y;py=x;}
   else if(o==R::LandscapeClockwise){px=1215-x;py=683-y;}
   else{px=x;py=y;}
   bool content=x>=left&&x<right&&y>=top&&y<bottom;
   const auto& b=display.body;
   bool submitted=px>=b[0]&&px<b[0]+b[2]&&py>=b[1]&&py<b[1]+b[3];
   assert(content==submitted);
   if(content){x0=std::min(x0,px);y0=std::min(y0,py);x1=std::max(x1,px+1);y1=std::max(y1,py+1);}
  }
  assert((display.body==std::array<int,4>{x0,y0,x1-x0,y1-y0}));
  r.setNextGray16ReaderRefresh(-10,-20,r.getScreenWidth()+20,r.getScreenHeight()+40);
  assert((display.body==std::array<int,4>{0,0,1216,684}));
 }
 struct LegacyDisplay{} legacy;configureGrayReader(legacy,0,0,1,1); // Older simulator HAL: no-op.
 configureMainTabTurn(legacy,0,true,0,0,1,1);
 struct OlderUi {bool called=false;void setNextGray16UiRefresh(int,int,int,int,uint8_t,uint8_t){called=true;}} older;
 configureGrayUi(older,0,0,1,1,8,32,true);assert(older.called);
}
'''
reader = (PORT / 'upstream/src/activities/reader/EpubReaderActivity.cpp').read_text()
assert 'if (refresh == HalDisplay::FAST_REFRESH && !panelTurn)' in reader
assert 'renderer.setNextGray16ReaderRefresh(orientedMarginLeft, orientedMarginTop,' in reader
with tempfile.TemporaryDirectory(prefix='plus-reader-geometry-') as folder:
    work = Path(folder)
    (work / 'test.cpp').write_text(program)
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(work / 'test.cpp'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('Actual reader/UI mapping: portrait, inverted portrait and both landscape rotations, clipping and legacy HAL passed (ASan/UBSan)')
