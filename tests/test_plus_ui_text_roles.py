"""Execute actual text adapters and navigation with distinct semantic ink roles."""
from pathlib import Path
import subprocess
import tempfile

APP = Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2/upstream'

def method(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

design = (APP / 'src/components/RickyDesignUi.h').read_text()
adapter = (APP / 'src/components/RickyUiTarget.h').read_text()
navigation = (APP / 'src/components/themes/inx/InxTheme.cpp').read_text()
program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "components/RickyDesignTokens.h"
#include "components/RickyPageLayout.h"
#include "activities/MainTab.h"
namespace fui {
struct Rect {int16_t x,y,width,height;bool empty()const{return width<=0||height<=0;}};
enum class Color {Black,White,DarkGray,LightGray,Transparent};
enum class TextAlign {Left,Center,Right};
struct TextStyle {int font=0;Color color=Color::Black;TextAlign align=TextAlign::Left;int maxLines=1;bool inverted=false;};
struct DrawTarget {virtual ~DrawTarget()=default;virtual void text(Rect,const char*,TextStyle)=0;};
struct GfxRendererTarget {static constexpr int FONT_BODY=0;};
template<class Target,class Callback>void layoutText(Target&,Rect r,const char* text,TextStyle,Callback callback){callback(text,r);}
}
namespace freeink {namespace ui=fui;}
struct GfxRenderer {
 mutable uint8_t ink=0;
 mutable std::vector<int> written;
 mutable std::vector<Rect> marks;
 bool gray=true;
 struct TextInkScope {const GfxRenderer&r;uint8_t old;
  TextInkScope(const GfxRenderer&r,uint8_t ink):r(r),old(r.ink){r.ink=ink;}
  ~TextInkScope(){r.ink=old;}
 };
 struct ClipScope {ClipScope(const GfxRenderer&,int,int,int,int){}};
 int getLineHeight(int)const{return 28;}
 int getTextWidth(int,const char* text)const{return int(std::strlen(text))*5;}
 bool isGrayscale16Active()const{return gray;}
 void fillGrayscale16Rect(int,int,int,int,int)const{}
 void fillRect(int x,int y,int w,int h,bool black)const{if(black)marks.emplace_back(x,y,w,h);}
 void drawText(int,int,int,const char*,bool)const{written.push_back(ink);}
};
struct Target {
 const GfxRenderer&r;
 int lineHeight(int)const{return 28;}
 void text(fui::Rect,const char*,fui::TextStyle)const{r.written.push_back(r.ink);}
};
class RickyUiTarget : public fui::DrawTarget {
 public:
 explicit RickyUiTarget(const GfxRenderer&r):target_{r},renderer_(r){}
 void setFont(int,int){}
 int lineHeight(int id)const{return target_.lineHeight(id);}
''' + method(adapter, 'void text(freeink::ui::Rect rect,') + r'''
 private:Target target_;const GfxRenderer&renderer_;
};
namespace RickyDesignUi {
namespace ds=ricky::ds;
constexpr int BODY=1,CAPTION=2;
int fontFor(const GfxRenderer&,int font,const char*){return font;}
''' + method(design, 'inline void text(') + '\n' + method(design, 'inline void secondaryText(') + r'''
}
constexpr int SMALL_FONT_ID=2,kPillHeight=48,kIconSize=40;
enum class StrId{STR_RICKY_NAV_HOME,STR_LIBRARY,STR_APPS_TITLE,STR_RICKY_STORAGE,STR_SETTINGS_TITLE};
struct {const char* get(StrId)const{return "Tab";}} I18N;
struct CrossPointSettings {static constexpr int INX_TAB_BOTTOM=1;};
struct {int inxTabPosition=1;} SETTINGS;
const uint8_t*iconForTab(MainTab){static uint8_t icon=0;return &icon;}
void drawInxIcon(const GfxRenderer&,const uint8_t*,int,int){}
struct InxTheme{void drawMainTabBar(const GfxRenderer&,Rect,MainTab)const;};
''' + method(navigation, 'void InxTheme::drawMainTabBar(') + r'''
int main(){
 using namespace ricky::ds::gray;
 assert(kInk==0 && kInk2>kInk && kInk2<kMute);
 GfxRenderer renderer;
 const fui::Rect box{0,0,400,80};
 RickyDesignUi::text(renderer,box,"date/title/book name");
 assert(renderer.written.back()==kInk && renderer.ink==0);
 for(const char* label:{"author","chapter/progress","statistics label","format/read state"}){
  RickyDesignUi::secondaryText(renderer,box,label);
  assert(renderer.written.back()==kInk2 && renderer.ink==0);
  RickyDesignUi::secondaryText(renderer,box,label,2,true);
  assert(renderer.written.back()==kPaper && renderer.ink==0);
 }
 RickyUiTarget target(renderer);
 fui::TextStyle count;count.color=fui::Color::DarkGray;
 target.text(box,"library count/category",count);
 assert(renderer.written.back()==kInk2 && renderer.ink==0);
 count.color=fui::Color::LightGray;
 target.text(box,"disabled",count);
 assert(renderer.written.back()==kMute && renderer.ink==0);
 // Actual five-tab drawing, both placements and B/W/gray: one selected black
 // label, four secondary labels, and a single mark over the selected hit cell.
 for(int width:{360,480,684,1216})for(int placement:{0,1})for(bool gray:{false,true})
 for(MainTab selected:MainTabs::values){
  GfxRenderer r;r.gray=gray;SETTINGS.inxTabPosition=placement;
  Rect bar{5,17,width,112};InxTheme{}.drawMainTabBar(r,bar,selected);
  assert(r.written.size()==5);
  for(int i=0;i<5;++i)assert(r.written[i]==(MainTabs::values[i]==selected?kInk:kInk2));
  assert(r.ink==0 && r.marks.size()==(gray?1:2));
  const auto mark=r.marks.back();
  const auto cell=MainTabs::tabBounds(MainTabs::indexOf(selected),width);
  assert(mark.x>=bar.x+cell.left && mark.x+mark.width<=bar.x+cell.right);
  assert(mark.width==48 && mark.height==4);
  assert(mark.y==(placement?bar.y:bar.y+bar.height-4));
 }
}
'''
with tempfile.TemporaryDirectory(prefix='plus-ui-roles-') as directory:
    work = Path(directory)
    (work / 'check.cpp').write_text(program)
    binary = work / 'check'
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-DFREEINK_DEVICE_METALIO_PLUS=1',
                    '-DRICKYOS_GRAY_PRESET=2', '-DCROSSMUX_UI_PROFILE_HIGH_DPI=1',
                    '-I' + str(APP / 'src'), str(work / 'check.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Actual text adapters: primary/secondary/disabled/inverted roles distinct; all 5 selected tabs across 4 widths, both placements and frame modes passed')
