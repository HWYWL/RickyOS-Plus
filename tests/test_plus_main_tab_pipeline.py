"""Execute the production Plus direct-gray controller with observable panel seams."""
from pathlib import Path
import sys
import unittest

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT / 'firmware/rickyos-plus-1.2/upstream'
sys.path.insert(0, str(ROOT / 'scripts/tests'))
from test_reading_ui_regressions import method, run_cpp


class MainTabPipelineTest(unittest.TestCase):
    def test_production_capture_and_single_submission(self):
        production = method((ROOT / 'src/activities/ActivityManager.cpp').read_text(),
                            'bool ActivityManager::renderDirectGray(')
        run_cpp(r'''
#include <atomic>
#include <cassert>
#include <utility>
#include "activities/MainTab.h"
#include "activities/MainTabMotion.h"
#define RICKYOS_PLUS_TAB_BENCH 0
#define RICKYOS_PERF_SD_LOG 0
template<class... A> void log(A...) {}
#define LOG_DBG(...) log(__VA_ARGS__)
#define LOG_ERR(...) log(__VA_ARGS__)
unsigned long millis() {return 1;}
struct {uint8_t mainTabMotion=1;} SETTINGS;
struct RenderLock {};
struct Renderer {
 bool active=false,capture=false,available=true,cleanup=false,success=true,adaptive=false;
 bool superseded=false,cancelledByPage=false;
 int begins=0,commits=0,cancels=0,motions=0,clears=0;
 bool right=false;
 uint8_t orientation=0;
 bool beginGrayscale16(){++begins;return active=available;}
 uint8_t getOrientation(){return orientation;}
 void setDisplayCapture(bool b){capture=b;}
 bool isGrayscale16Active(){return active;}
 void cancelGrayscale16(){++cancels;active=false;}
 void setNextGray16UiRefresh(int,int,int,int,uint8_t,uint8_t,bool a){adaptive=a;}
 bool nextRefreshNeedsCleanup(){return cleanup;}
 void setNextMainTabTurn(int,int,int,int,bool r){assert(!capture);++motions;right=r;}
 bool commitGrayscale16(){assert(!capture);++commits;active=false;return success;}
 void clearNextRefreshOverride(){++clears;}
};
struct Activity {
 Renderer* r;
 MainTab tab=MainTab::Recent;
 bool root=true;
 int renders=0;
 bool usesMainTabBar(){return root;}
 MainTab mainTab(){return tab;}
 MainTabLayout mainTabLayout(){
   MainTabLayout result{Rect{}, Rect{}, Rect{}};result.tabBar=Rect{0,1120,684,96};result.content=Rect{12,60,660,1020};return result;
 }
 void render(RenderLock&&){assert(r->capture);++renders;if(r->cancelledByPage)r->active=false;}
};
struct ActivityManager {
 Renderer renderer;
 Activity activity{&renderer};
 Activity* currentActivity=&activity;
 MainTabMotion mainTabMotion_;
 struct {int cleanup=0;void setNextGray16Profile(uint8_t p){assert(p==2);++cleanup;}} display;
 std::atomic<uint32_t> renderRequestGeneration{0};
 std::atomic<bool> requestedUpdate{false};
 bool switching=false;
 bool isSwitchPending(){return switching;}
 bool renderDirectGray(RenderLock&&);
 void draw(MainTab t){activity.tab=t;assert(renderDirectGray(RenderLock{}));}
};
''' + production + r'''
int main() {
 ActivityManager m;
 m.draw(MainTab::Recent);assert(m.renderer.motions==0 && m.renderer.commits==1);
 m.draw(MainTab::Library);assert(m.renderer.motions==1 && m.renderer.right && m.renderer.adaptive);
 m.draw(MainTab::Recent);assert(m.renderer.motions==2 && !m.renderer.right);
 m.draw(MainTab::Recent);assert(m.renderer.motions==2); // cover/background redraw is not a navigation.
 SETTINGS.mainTabMotion=0;m.draw(MainTab::Library);
 assert(m.renderer.motions==2 && !m.renderer.adaptive);
 SETTINGS.mainTabMotion=1;m.renderer.cleanup=true;m.draw(MainTab::Apps);
 assert(m.renderer.motions==2 && m.display.cleanup==1);
 m.renderer.cleanup=false;m.draw(MainTab::Settings);assert(m.renderer.motions==3);
 // Superseded captures have no optical side effect or history advancement.
 int before=m.renderer.commits;
 m.requestedUpdate=true;m.draw(MainTab::Recent);
 assert(m.renderer.commits==before && m.renderer.cancels==1);
 m.requestedUpdate=false;m.draw(MainTab::Library);
 assert(m.renderer.motions==4 && !m.renderer.right);
 // Failed and page-cancelled frames cannot seed a later animation.
 m.renderer.success=false;assert(!m.renderDirectGray(RenderLock{}));
 m.renderer.success=true;m.draw(MainTab::Apps);assert(m.renderer.motions==4);
 m.renderer.cancelledByPage=true;m.draw(MainTab::Settings);
 m.renderer.cancelledByPage=false;m.draw(MainTab::Recent);assert(m.renderer.motions==4);
 m.activity.root=false;m.draw(MainTab::Recent);
 m.activity.root=true;m.draw(MainTab::Library);assert(m.renderer.motions==4);
 m.renderer.available=false;assert(!m.renderDirectGray(RenderLock{}));
 m.renderer.available=true;m.draw(MainTab::Settings);assert(m.renderer.motions==4);
 assert(!m.renderer.capture && m.renderer.commits==m.renderer.clears+1); // one failed commit
}
''', include_dirs=(ROOT / 'src',))


if __name__ == '__main__':
    unittest.main()
