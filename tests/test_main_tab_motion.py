#!/usr/bin/env python3
"""Exercise actual tab-history policy and production orientation mapping."""
import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2/upstream/scripts/tests'))
from test_reading_ui_regressions import ROOT, run_cpp, method

class MainTabMotionTest(unittest.TestCase):
    def test_history_all_twenty_routes(self):
        run_cpp(r'''
#include <cassert>
#include "activities/MainTabMotion.h"
int main() {
  Rect content{12,40,660,1040};
  for(int o=0;o<4;o++) for(int from=0;from<5;from++) for(int to=0;to<5;to++) {
    MainTabMotion motion;
    assert(motion.directionTo(to,o,content,true,false)==0);
    motion.presented(from,o,content,true);
    assert(motion.directionTo(to,o,content,true,false)==(to==from?0:to>from?1:-1));
    assert(motion.directionTo(to,o,content,false,false)==0);
    assert(motion.directionTo(to,o,content,true,true)==0);
    assert(motion.directionTo(to,(o+1)%4,content,true,false)==0);
    assert(motion.directionTo(to,o,Rect{12,41,660,1039},true,false)==0);
    assert(motion.directionTo(5,o,content,true,false)==0);
    assert(motion.directionTo(-1,o,content,true,false)==0);
    motion.presented(to,o,content,false);
    assert(motion.directionTo(from,o,content,true,false)==0);
    motion.presented(from,o,content,true);
    motion.invalidate();
    assert(motion.directionTo(to,o,content,true,false)==0);
    motion.presented(-1,o,content,true);
    assert(motion.directionTo(to,o,content,true,false)==0);
  }
}
''', include_dirs=(ROOT / "src",))

    def test_actual_geometry_map(self):
        source = (ROOT / "lib/GfxRenderer/GfxRenderer.cpp").read_text()
        production = method(source, "void GfxRenderer::mapGrayRefreshArea(")
        run_cpp(r'''
#include <algorithm>
#include <cassert>
struct GfxRenderer {
  enum Orientation {Portrait,LandscapeClockwise,PortraitInverted,LandscapeCounterClockwise};
  Orientation orientation;
  int panelWidth=1216,panelHeight=684;
  int getScreenWidth() const {return orientation==Portrait||orientation==PortraitInverted?684:1216;}
  int getScreenHeight() const {return orientation==Portrait||orientation==PortraitInverted?1216:684;}
  void mapGrayRefreshArea(int&,int&,int&,int&) const;
};
''' + production + r'''
int main(){
  for(auto o:{GfxRenderer::Portrait,GfxRenderer::LandscapeClockwise,
               GfxRenderer::PortraitInverted,GfxRenderer::LandscapeCounterClockwise}) {
    GfxRenderer r{o};
    int x=12,y=40,w=r.getScreenWidth()-24,h=r.getScreenHeight()-168;
    r.mapGrayRefreshArea(x,y,w,h);
    assert(x>=0&&y>=0&&w>0&&h>0&&x+w<=1216&&y+h<=684);
    if(o==GfxRenderer::Portrait) assert(x==40&&y==12&&w==1048&&h==660);
    if(o==GfxRenderer::PortraitInverted) assert(x==128&&y==12&&w==1048&&h==660);
    if(o==GfxRenderer::LandscapeClockwise) assert(x==12&&y==128&&w==1192&&h==516);
    if(o==GfxRenderer::LandscapeCounterClockwise) assert(x==12&&y==40&&w==1192&&h==516);
    x=-7;y=-9;w=r.getScreenWidth()+20;h=r.getScreenHeight()+30;
    r.mapGrayRefreshArea(x,y,w,h);
    assert(x==0&&y==0&&w==1216&&h==684);
  }
}
''')

    def test_settings_are_independent_and_on_by_default(self):
        source = (ROOT / "src/CrossPointSettings.h").read_text()
        # Both motions start on (page turn: skim); a saved choice, off included, is kept.
        self.assertIn("uint8_t nativePageTurn = 1", source)
        self.assertIn("uint8_t mainTabMotion = 1", source)
        save = (ROOT / "src/CrossPointSettings.cpp").read_text()
        self.assertIn('doc["mainTabMotion"] = mainTabMotion', save)
        self.assertIn('doc["nativePageTurn"] = nativePageTurn', save)
        self.assertIn('(doc["mainTabMotion"] | 1) == 1 ? 1 : 0', save)
        self.assertIn('nativePageTurn = doc["nativePageTurn"] | static_cast<uint8_t>(1);', save)
        self.assertIn('if (nativePageTurn > 3) nativePageTurn = 1;', save)
        policy = (ROOT / 'src/activities/MainTabMotion.h').read_text()
        self.assertIn('#define RICKYOS_MAIN_TAB_MOTION_ENABLED 1', policy)
        menu = (ROOT / 'src/SettingsList.h').read_text()
        self.assertIn('#if RICKYOS_MAIN_TAB_MOTION_ENABLED', menu)
        manager = (ROOT / 'src/activities/ActivityManager.cpp').read_text()
        self.assertIn('RICKYOS_MAIN_TAB_MOTION_ENABLED && SETTINGS.mainTabMotion != 0', manager)

if __name__ == "__main__":
    unittest.main()
