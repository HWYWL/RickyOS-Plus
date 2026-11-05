#!/usr/bin/env python3
"""Host acceptance for the actual native scheduler and C feed-line adapter."""
import pathlib
import re
import subprocess
import tempfile
import unittest
import sys

# Reuse the restored application test seam, not the repository root app.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "firmware/rickyos-plus-1.2/upstream/scripts/tests"))
from test_reading_ui_regressions import run_cpp, method

PORT = pathlib.Path(__file__).resolve().parents[1]
ROOT = PORT / "firmware/rickyos-plus-1.2/upstream"
SDK = PORT / "firmware/rickyos-plus-1.2/components/metalio_board/display"

class NativeTurnTest(unittest.TestCase):
    def test_actual_page_turn_origin_lifecycle(self):
        reader=(ROOT/'src/activities/reader/EpubReaderActivity.cpp').read_text()
        production=method(reader,'bool EpubReaderActivity::pageTurnWithOrigin(')
        run_cpp(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include "activities/reader/NativeTurnRequest.h"
struct Section {int currentPage=1,pageCount=5;bool building=false;bool isBuilding() {return building;}};
struct Book {int getSpineItemsCount() {return 3;}};
struct RenderLock {~RenderLock() {}};
struct Stats {void noteActivity() {}} READING_STATS;
unsigned long millis() {return 50;}
struct EpubReaderActivity {
  std::unique_ptr<Section> section=std::make_unique<Section>();
  Book book;Book* epub=&book;
  std::atomic<uint32_t> pendingNativeTurn_{0};
  int nextPageNumber=0,currentSpineIndex=0;unsigned long lastPageTurnTime=0;
  std::optional<uint16_t> pendingPageJump;
  void clearDeferredReposition() {}
  bool pageTurnWithOrigin(bool,int,int);
};
''' + production + r'''
int main() {
  EpubReaderActivity r;
  assert(r.pageTurnWithOrigin(true,0,1023) && r.section->currentPage==2);
  auto p=r.pendingNativeTurn_.exchange(0);
  assert(nativePageTurn::direction(p)==1 && nativePageTurn::originX(p)==0 && nativePageTurn::originY(p)==1023);
  assert(r.pageTurnWithOrigin(false,900,100) && r.section->currentPage==1);
  p=r.pendingNativeTurn_.exchange(0);
  assert(nativePageTurn::direction(p)==-1 && nativePageTurn::originX(p)==900);
  assert(r.pageTurnWithOrigin(true,-1,-1));
  assert(nativePageTurn::originX(r.pendingNativeTurn_)==-1);
  // Chapter crossing cannot retain an earlier tap's request.
  r.section->currentPage=4;
  assert(r.pageTurnWithOrigin(true,400,500) && !r.section && r.pendingNativeTurn_==0);
  r.pendingNativeTurn_=nativePageTurn::pack(1,400,500);
  assert(!r.pageTurnWithOrigin(false,400,500) && r.pendingNativeTurn_==0);
  r.section=std::make_unique<Section>();r.section->currentPage=0;
  assert(r.pageTurnWithOrigin(false,100,200) && !r.section && r.currentSpineIndex==0);
  assert(r.pendingNativeTurn_==0);
}
''', include_dirs=(ROOT / 'src',))

    def test_atomic_request_and_touch_origin(self):
        run_cpp(r'''
#include <cassert>
#include <atomic>
#include "activities/reader/NativeTurnRequest.h"
using namespace nativePageTurn;
int main() {
  static_assert(std::atomic<uint32_t>::is_always_lock_free);
  assert(pack(0,1,1)==0 && direction(0)==0);
  for(int d:{-1,1}) {
    auto r=pack(d); assert(direction(r)==d && originX(r)==-1 && originY(r)==-1);
    for(int x=0;x<=1023;x+=31) for(int y=0;y<=1023;y+=29) {
      r=pack(d,x,y); assert(direction(r)==d && originX(r)==x && originY(r)==y);
    }
    r=pack(d,1023,0); assert(originX(r)==1023 && originY(r)==0);
    assert(originX(pack(d,1024,0))==-1 && originY(pack(d,0,-1))==-1);
  }
  assert(normalize(0,684)==0 && normalize(683,684)==1023 && normalize(1215,1216)==1023);
  assert(normalize(684,684)==-1 && normalize(-1,684)==-1 && normalize(0,1)==-1);
  std::atomic<uint32_t> queued{pack(1,100,800)};
  auto shown=queued.exchange(0);
  queued=pack(-1,900,100);
  assert(direction(shown)==1 && originX(shown)==100 && originY(shown)==800);
  auto next=queued.exchange(0);
  assert(direction(next)==-1 && originX(next)==900 && originY(next)==100 && queued==0);
}
'''.replace('#include <atomic>', '#include <atomic>\n#include <initializer_list>'), include_dirs=(ROOT / 'src',))
        reader=(ROOT/'src/activities/reader/EpubReaderActivity.cpp').read_text()
        self.assertIn('const auto request = pendingManualTurn;',reader)
        self.assertIn('pendingManualTurn = nativePageTurn::pack(prevTriggered ? -1 : 1, originX, originY);',reader)
        self.assertIn('mappedInput.wasScreenTapped(touchX, touchY)',reader)


    def test_production_c_with_sanitizers(self):
        with tempfile.TemporaryDirectory(prefix="ricky-native-turn-") as temp:
            binary = pathlib.Path(temp) / "native-turn"
            (pathlib.Path(temp) / "esp_log.h").write_text('#define ESP_LOGD(...) ((void)0)\n')
            subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200112L", "-DCONFIG_IDF_TARGET_ESP32S31=1", "-O1", "-g",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-I", temp, "-I", str(SDK), str(PORT / "tests/native_turn_host.c"),
                            str(SDK / "waveforms/my_waveform.c"), str(SDK / "waveforms/my_waveform_trim.c"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_on_by_default_and_single_page_only(self):
        settings = (ROOT / "src/CrossPointSettings.h").read_text()
        self.assertIn("uint8_t nativePageTurn = 1", settings)  # skim
        reader = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text()
        self.assertIn("pendingNativeTurn_.exchange(0)", reader)
        self.assertIn("nativeTurnDirection != 0 && previousNativeText && !pageHasImages", reader)
        self.assertIn("refresh == HalDisplay::FAST_REFRESH && !panelTurn", reader)
        self.assertIn("pendingNativeTurn_ = nativePageTurn::pack(-1, originX, originY)", reader)
        self.assertIn("pendingNativeTurn_ = nativePageTurn::pack(1, originX, originY)", reader)
        renderer = (SDK / "render.c").read_text()
        self.assertIn("s_next_native_turn = (RickyNativeTurn){0}", renderer)
        self.assertIn("ricky_native_end(&render_context)", renderer)

    def test_menu_glyph_coverage(self):
        text = ("翻页动效（实验）掠页柔弧涟漪主界面切换动效"
                "图库壁纸设为待机关机阅读背景选择关闭浅中深纸样图片横线方格网点"
                "默认我的图片使用中传图片时间完整铺满原图高对比反色书籍封面日历时钟透明叠图空白全部更换图片")
        for name in ("ricky_caption_10_regular", "ricky_headline_14_medium", "ricky_title_16_bold"):
            source = (ROOT / "lib/EpdFont/builtinFonts" / (name + ".h")).read_text()
            intervals = source.split(name + "Intervals[] = {", 1)[1].split("};", 1)[0]
            ranges = [(int(a, 16), int(b, 16)) for a, b in
                      re.findall(r"\{\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+),", intervals)]
            for char in text:
                self.assertTrue(any(a <= ord(char) <= b for a, b in ranges), (name, char))

if __name__ == "__main__":
    unittest.main()
