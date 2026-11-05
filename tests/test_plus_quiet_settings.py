"""Execute the product row drawer: short overflow hint and quiet System arrows."""
from pathlib import Path
import subprocess
import tempfile

APP = Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2/upstream'

def extract(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

rows = (APP / 'src/components/RickyRowList.h').read_text()
settings = (APP / 'src/activities/settings/SettingsActivity.cpp').read_text()
program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>
#include "components/RickyDesignTokens.h"
namespace freeink {struct Icon {};}
namespace fui {
struct Rect {int16_t x=0,y=0,width=0,height=0;bool empty()const{return width<=0||height<=0;}
 int right()const{return x+width;}int bottom()const{return y+height;}};
enum class Color {DarkGray};
struct TextStyle {Color color=Color::DarkGray;};
enum {StateSelected,StateNormal};
struct ListItem {const char*label="label";const char*subtitle=nullptr;const char*value="value";
 int icon=0;bool toggle=false,toggleChecked=false;int actionValue=0;};
int bitmapFromIcon(const freeink::Icon&){return 1;}
struct ListProps {ListItem*items=nullptr;int count=0,topIndex=0,selectedIndex=-1,action=77,inputMask=3;
 bool scrollIndicator=true;int rowHeight=96,rowGap=8,sidePadding=16,textGap=16,iconSize=32,rowInset=0;
 int toggleKnobRadius=0,toggleKnobInset=0,toggleHeight=36,rowStyles=0;
 void*nav=nullptr;TextStyle labelText,subtitleText;};
struct SettingRowProps {const char*label=nullptr,*subtitle=nullptr,*value=nullptr;
 int icon=0,iconSize=0,textGap=0,action=0,valueId=0,inputMask=0,styles=0,state=0,radius=0,sidePadding=0,minTouchSize=0;
 bool drawChevron=false;TextStyle labelText,subtitleText,valueText;};
struct Drawn {Rect row;int action,id;bool chevron,toggle;};
struct Frame {std::vector<Drawn>rows;};
void settingRow(Frame&frame,Rect row,const SettingRowProps&p){frame.rows.push_back({row,p.action,p.valueId,p.drawChevron,false});}
void list(Frame&frame,Rect row,const ListProps&p){assert(p.count==1&&!p.scrollIndicator&&p.nav==nullptr);
 frame.rows.push_back({row,p.action,p.items[0].actionValue,false,true});}
struct ListNav {int drawnCount=0,top=-1,visible=-1;bool selected=false;
 void onListRendered(int t,int n,bool s){top=t;visible=n;selected=s;}};
}
namespace freeink {namespace ui=fui;}
struct GfxRenderer {mutable std::vector<fui::Rect> fills;};
struct UiAppHost {struct UiScreen {fui::Rect area{24,100,600,1000};fui::Frame f;
 struct Theme {fui::TextStyle smallText;} t;
 fui::Rect body()const{return area;}fui::Frame&frame(){return f;}Theme&theme(){return t;}};};
struct UITheme {static UITheme&getInstance(){static UITheme t;return t;}bool showSelectionCursor()const{return true;}};
namespace RickyDesignUi {void fill(const GfxRenderer&r,fui::Rect rect,int tone){
 assert(tone==ricky::ds::gray::kLine);if(!rect.empty())r.fills.push_back(rect);}}
namespace RickyRowList {
''' + extract(rows, 'struct RowStyle') + ';\n' + extract(rows, 'inline fui::Rect scrollThumb(') + '\n' + extract(rows, 'template <typename StyleFn>') + r'''
}
struct SettingInfo {bool opens=true;};
struct SettingsActivity {
 std::vector<SettingInfo>systemSettings{{true},{false},{true}},other{{true},{false},{true}};
 std::vector<SettingInfo>*currentSettings=&systemSettings;int settingsCount=3;
 bool opensPage(const SettingInfo&s){return s.opens;}
 RickyRowList::RowStyle style(int index){
  const bool quietSystem=currentSettings==&systemSettings;
  auto select=''' + extract(settings, '[this, quietSystem]') + r''';return select(index);
 }
};
int main(){
 SettingsActivity activity;
 for(int i=0;i<3;++i)assert(!activity.style(i).chevron);
 activity.currentSettings=&activity.other;
 assert(activity.style(0).chevron&&!activity.style(1).chevron&&activity.style(2).chevron);
 std::vector<fui::ListItem>items(24);
 for(int i=0;i<24;++i){items[i].actionValue=i+7;items[i].toggle=i%3==0;items[i].subtitle=i%2?"detail":nullptr;}
 for(int count:{0,1,8,9,12,24})for(int height:{1,100,400,1100})for(bool indicator:{false,true}){
  for(int top=0;top<std::max(1,count);++top){
   UiAppHost::UiScreen screen;screen.area.height=height;
   GfxRenderer renderer;fui::ListNav nav;fui::ListProps props;
   props.items=items.data();props.count=count;props.topIndex=top;props.scrollIndicator=indicator;props.selectedIndex=top;
   RickyRowList::draw(screen,renderer,nav,props,[](int){return RickyRowList::RowStyle{};},7);
   assert(nav.drawnCount==count&&nav.top==top&&nav.visible==int(screen.f.rows.size()));
   for(int i=0;i<nav.visible;++i){const auto&row=screen.f.rows[i];
    assert(row.action==77&&row.id==top+i+7&&!row.chevron);
    assert(row.row.y>=screen.area.y&&row.row.bottom()<=screen.area.bottom());
    if(i)assert(row.row.y-screen.f.rows[i-1].row.bottom()>=8);
   }
   const bool overflow=indicator&&nav.visible>0&&count>nav.visible;
   assert(renderer.fills.size()==(overflow?1u:0u));
   for(const auto&thumb:renderer.fills){
    assert(thumb.height<=64&&thumb.height<=height&&thumb.width<=2);
    assert(thumb.y>=screen.area.y&&thumb.bottom()<=screen.area.bottom());
    assert(thumb.right()==screen.area.right());
   }
  }
 }
 fui::Rect body{10,20,200,1000};
 auto first=RickyRowList::scrollThumb(body,20,8,0),last=RickyRowList::scrollThumb(body,20,8,12);
 assert(first.y==body.y&&last.bottom()==body.bottom());
 assert(RickyRowList::scrollThumb(body,8,8,0).empty());
}
'''
with tempfile.TemporaryDirectory(prefix='plus-quiet-settings-') as directory:
    work = Path(directory)
    (work / 'check.cpp').write_text(program)
    binary = work / 'check'
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-DFREEINK_DEVICE_METALIO_PLUS=1',
                    '-DRICKYOS_GRAY_PRESET=2', '-I' + str(APP / 'src'),
                    str(work / 'check.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Actual row drawer: no full rail, <=64 px overflow-only hint, hidden indicator, variable rows and action IDs preserved; System chevrons off and other categories unchanged; ASan/UBSan passed')
