"""Compile the actual recorder UI and actual app-grid hit geometry with host fakes."""
from pathlib import Path
import subprocess,tempfile,re
root=Path(__file__).resolve().parents[1]
u=root/'firmware/rickyos-plus-1.2/upstream'
with tempfile.TemporaryDirectory() as tmp:
 d=Path(tmp);(d/'activities').mkdir();(d/'components').mkdir()
 (d/'HalRecorder.h').write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
namespace HalRecorder {
enum class State:uint8_t { Idle,Recording,Playing,Saved,Error };
struct Entry { char name[40]{};uint32_t seconds=0; };
inline State current=State::Idle;
inline bool waiting=false;
inline int starts=0,plays=0,stops=0;inline uint32_t elapsed=0;inline bool complete=false;inline size_t total=12;
inline State state(){return current;} inline bool active(){return current==State::Recording||current==State::Playing;}
inline uint32_t seconds(){return elapsed;}
inline bool preparing(){return current==State::Recording && waiting;}
inline bool startRecording(){++starts;current=State::Recording;waiting=true;elapsed=0;return true;}
inline bool play(const char*,uint8_t){++plays;current=State::Playing;return true;}
inline void stop(){if(active()){++stops;current=State::Saved;}}
inline void poll(){if(complete){current=State::Saved;complete=false;}}
inline size_t list(size_t offset,Entry* e,size_t cap,bool& more){size_t n=0;while(offset+n<total&&n<cap){snprintf(e[n].name,40,"REC_%03zu.wav",offset+n);e[n].seconds=offset+n;++n;}more=offset+n<total;return n;}
}
''')
 (d/'CrossPointSettings.h').write_text('#pragma once\nstruct FakeSettings { uint8_t speakerVolume=35,plusTouchFeedback=1; }; inline FakeSettings settings;\n#define SETTINGS settings\n')
 ids=re.findall(r'^(STR_REC_\w+):', (u/'lib/I18n/translations/english.yaml').read_text(),re.M)
 (d/'I18n.h').write_text('#pragma once\nenum class StrId {'+','.join(ids)+'};\ninline const char* trans(StrId){return "localized";}\n#define tr(id) trans(StrId::id)\n')
 (d/'components/UITheme.h').write_text('''#pragma once
#include <RickyPageLayout.h>
struct GfxRenderer { int width=684; int getScreenWidth()const{return width;} };
struct Metrics { int topPadding=16,headerHeight=100,buttonHintsHeight=88,verticalSpacing=16; };
struct UITheme { static UITheme& getInstance(){static UITheme t;return t;}const Metrics& getMetrics(){static Metrics m;return m;}void drawHeader(GfxRenderer&,Rect,const char*,const char*){} };
#define GUI UITheme::getInstance()
''')
 (d/'activities/UiListActivity.h').write_text('''#pragma once
#include <components/UITheme.h>
#include <cstdint>
#include <cassert>
namespace freeink::ui {
using ActionId=int;constexpr int InputTouch=1;
struct Insets{int16_t t,r,b,l;};
struct ListItem {const char* label=nullptr;const char* subtitle=nullptr;const char* value=nullptr;int16_t actionValue=0;};
struct ListProps{const ListItem* items=nullptr;uint16_t count=0;int action=0,inputMask=0;};
}
struct UiScreen {freeink::ui::ListProps props;void setContentMargin(freeink::ui::Insets){}void spacer(int16_t){}void list(freeink::ui::ListProps p){props=p;}};
struct MappedInputManager {enum class Button {Confirm};bool held=false;int pulses=0;void pulseHapticFeedback(){++pulses;}bool isPressed(Button){return held;}};
struct FakeActivityManager { int appVisits=0;void goToApps(){++appVisits;} }; inline FakeActivityManager activityManager;
struct RenderLock {
 enum class Mode{Try}; inline static bool held=false;bool owned=false;
 RenderLock(){assert(!held);held=owned=true;}
 explicit RenderLock(Mode){if(!held)held=owned=true;}
 template<class T> explicit RenderLock(T&){assert(!held);held=owned=true;}
 ~RenderLock(){if(owned)held=false;}
 bool ownsLock(){return owned;}
};
class Activity { public:virtual ~Activity()=default;virtual void onEnter(){}virtual void onExit(){}virtual void prepareForSleep(){}virtual bool preventAutoSleep(){return false;} };
inline int renderRequests=0;
class UiListActivity:public Activity {
 protected:
 GfxRenderer& renderer;MappedInputManager& mappedInput;
 struct {int selected=0;void reset(){selected=0;}} nav;
 struct {void clearTapFlash(){}} app;
 constexpr static int ACTION_ROW=1;
 virtual int listCount()const=0;virtual void buildScreen(UiScreen&)=0;virtual void activateIndex(int)=0;
 virtual bool handleCustomInput(){return false;}virtual void onBackButton(){}virtual void drawChrome(){}
 void syncListViewport(UiScreen&,freeink::ui::ListProps&){}void requestUpdate(){++renderRequests;}
 public:
 UiListActivity(const char*,GfxRenderer& r,MappedInputManager& i):renderer(r),mappedInput(i){}
 int rowCount(){return listCount();}void select(int i){activateIndex(i);}void back(){onBackButton();}bool tick(){return handleCustomInput();}
 void draw(UiScreen& s){drawChrome();buildScreen(s);}
};
''')
 source=(u/'src/activities/apps/AppsMenuActivity.cpp').read_text()
 helper=source[source.index('constexpr int appGridPageStart'):source.index('constexpr int kAppCount')]
 size_start=source.index('int appGridPageSize(')
 helper+=source[size_start:source.index('\nvoid drawRickyAppIcon(',size_start)]
 start=source.index('int AppsMenuActivity::iconIndexFromPoint(');end=source.index('\nvoid AppsMenuActivity::openSelected()',start)
 hit=source[start:end]
 (d/'test.cpp').write_text('''#include <RecorderActivity.h>
#include <CrossPointSettings.h>
#include <cassert>
#include <algorithm>
#include <RickyPageLayout.h>
namespace ricky::ds::space { constexpr int kGutter=20; }
constexpr int kAppPillHeight=112;
'''+helper+'''
class AppsMenuActivity {public:struct {int selected=0;}nav;int count=7;Rect rect{40,160,604,448};Rect appContentRect()const{return rect;}int getVisibleAppCount()const{return count;}int iconIndexFromPoint(int,int)const;};
'''+hit+'''
int main(){
 GfxRenderer r;MappedInputManager i;RecorderActivity a(r,i);UiScreen s;
 i.held=true;a.onEnter();assert(a.tick());i.held=false;assert(a.tick());assert(!a.tick());
 assert(a.rowCount()==7);a.draw(s);assert(s.props.count==7);
 a.select(6);assert(a.rowCount()==8);a.select(7);assert(a.rowCount()==4); // page 3: two files and previous
 a.select(3);assert(a.rowCount()==8); // previous
 a.select(1);assert(HalRecorder::plays==1 && a.preventAutoSleep());a.tick();assert(a.rowCount()==1);a.draw(s);assert(s.props.count==1);
 a.select(1);assert(HalRecorder::plays==1); // stale file index cannot start playback while active
 a.back();assert(!a.preventAutoSleep() && activityManager.appVisits==0);
 a.select(0);int requests=renderRequests;a.tick();assert(renderRequests==requests);assert(HalRecorder::starts==1 && a.rowCount()==1);{RenderLock held;a.prepareForSleep();assert(a.tick());}a.tick();assert(!a.preventAutoSleep());
 a.select(0);assert(HalRecorder::preparing());requests=renderRequests;a.tick();assert(renderRequests==requests);assert(i.pulses==0);
 HalRecorder::waiting=false;a.tick();assert(renderRequests==requests+1 && i.pulses==1 && HalRecorder::seconds()==0);a.tick();assert(i.pulses==1);a.back();
 a.select(0);a.back();a.tick();assert(i.pulses==1); // preparation cancellation never signals ready
 a.select(0);HalRecorder::current=HalRecorder::State::Error;a.tick();assert(i.pulses==1); // setup failure never signals ready
 SETTINGS.plusTouchFeedback=0;a.select(0);HalRecorder::waiting=false;a.tick();assert(i.pulses==1);a.back();SETTINGS.plusTouchFeedback=1;
 a.select(0);HalRecorder::complete=true;a.tick();assert(!a.preventAutoSleep() && a.rowCount()>1);
 a.select(0);{RenderLock held;a.onExit();}assert(!HalRecorder::active());a.back();assert(activityManager.appVisits==1);
 AppsMenuActivity grid;
 assert(appGridPageSize(Rect{40,160,604,976},7)==9); // all Plus entries on the portrait page
 assert(appGridPageSize(Rect{40,160,1136,448},7)==6); // constrained height still paginates
 for(int width:{604,1136})for(int height:{448,976})for(int count=1;count<=20;++count)for(int selected=0;selected<count;++selected){
   grid.rect=Rect{40,160,width,height};grid.count=count;grid.nav.selected=selected;
   const int capacity=appGridPageSize(grid.rect,count);const int start=appGridPageStart(selected,count,capacity);
   assert(start<=selected && selected<start+capacity);
   for(int slot=0;slot<capacity;++slot){
     auto cell=RickyPageLayout::cell(grid.rect,slot,capacity,20,3);
     assert(cell.x>=grid.rect.x && cell.y>=grid.rect.y && cell.x+cell.width<=grid.rect.x+grid.rect.width && cell.y+cell.height<=grid.rect.y+grid.rect.height);
     int got=grid.iconIndexFromPoint(cell.x+cell.width/2,cell.y+cell.height/2);assert(got==(start+slot<count?start+slot:-1));
     if(slot%3<2)assert(grid.iconIndexFromPoint(cell.x+cell.width+10,cell.y+cell.height/2)==-1);
   }
   assert(grid.iconIndexFromPoint(0,0)==-1);
 }
}
''')
 subprocess.run(['c++','-std=c++20','-DFREEINK_DEVICE_METALIO_PLUS=1','-fsanitize=address,undefined','-g',f'-I{d}',f'-I{u}/src/components',f'-I{u}/src/activities/apps/recorder',str(d/'test.cpp'),str(u/'src/activities/apps/recorder/RecorderActivity.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
print('Recorder UI: inherited Confirm gate, start/stop/back/sleep/completion, stale file taps, paged list; actual adaptive app-grid hit geometry, gap rejection and seven-entry portrait page for 1..20 apps/portrait/landscape passed')
