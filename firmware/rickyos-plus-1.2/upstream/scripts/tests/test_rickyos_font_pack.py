"""Offline guards for font-package roles, coverage, bounds and source identity."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("pack", ROOT / "scripts/build_rickyos_font_pack.py")
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


def fixture():
    header = struct.pack("<8sHHB19s", b"CPFONT\0\0", 4, 1, 2, bytes(19))
    toc = b"".join(struct.pack("<B3xIIBhhHHBBBI4x", style, 1, 1, 3, 2, -1,
                                0, 0, 0, 0, 0, 96 + style * 29) for style in (0, 1))
    section = struct.pack("<III", 65, 65, 0) + struct.pack("<BBHhhH2xI", 1, 1, 16, 0, 1, 1, 0) + b"\xc0"
    return bytearray(header + toc + section * 2)


class RickyFontPackTest(unittest.TestCase):
    def test_final_font_requires_consent_and_oversized_fonts_stay_on_sd(self):
        source = (ROOT / "src/activities/settings/TextSettingsActivity.cpp").read_text()
        run_cpp(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <string>
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
namespace SdCardFontCache {
enum class Result { Ok, AlreadyCached, TooLarge, Oom, InvalidFont };
const char* resultName(Result) { return ""; }
Result next=Result::Ok;
Result preflight(const char*) { return next; }
}
struct Settings {
  uint8_t sdFontFlashPreload=0, fontPointSize=18;
  char sdFontFamilyName[64]="RickySerif";
  void saveToFile() {}
} settings;
#define SETTINGS settings
struct Family { bool vector=false; } family;
struct Registry { const Family* findFamily(const char*) { return &family; } } registry;
struct SdCardFontFileInfo { std::string path="/fonts/RickySerif/RickySerif_18.cpfont"; } file;
struct SdSystem {
  int loads=0, releases=0;
  void ensureLoaded(int,bool) { ++loads; }
  int vectorReleases=0;
  void releaseLoadedFont(int, bool includeVector=false) { ++releases; vectorReleases+=includeVector; }
} sdFontSystem;
enum class StrId { STR_FONT_PRELOAD_START, STR_FONT_PRELOAD_SKIP, STR_FONT_PRELOAD_CONFIRM };
struct TextSettingsActivity {
  enum class FontLoadState { Idle };
  enum class ExitPrompt { None, Waiting, Accepted };
  std::atomic<FontLoadState> fontLoadState_{FontLoadState::Idle};
  ExitPrompt exitPrompt_=ExitPrompt::None;
  bool exitPromptWaitForBackRelease_=false;
  int currentFamilyIndex_=0, renderer=0, attempts=0, errors=0, completions=0, prompts=0;
  Registry* registry_=&registry;
  struct Input { enum class Button { Back }; bool isPressed(Button) { return false; } } mappedInput;
  struct Popup {
    TextSettingsActivity* owner;
    template<class Callback> void show(StrId,const StrId*,int,int,Callback) { ++owner->prompts; }
  } optionPopup_{this};
  struct RenderLock { explicit RenderLock(TextSettingsActivity&) {} };
  const SdCardFontFileInfo* fontFileForFamily(int,uint8_t) { return &file; }
  SdCardFontCache::Result preloadFont(const SdCardFontFileInfo&,const char*) {
    ++attempts; return SdCardFontCache::Result::Ok;
  }
  void completeExit() { ++completions; }
  void showPreloadFailure(SdCardFontCache::Result) { ++errors; }
  void requestUpdate() {}
  void finishFinalFont(bool);
};
using MappedInputManager=TextSettingsActivity::Input;
''' + method(source, 'void TextSettingsActivity::finishFinalFont(') + r'''
int main() {
  using R=SdCardFontCache::Result;
  // Vector (TTF) families take the same consent and acceleration path as .cpfont.
  for(bool vector : {false,true})
  for(auto check : {R::Ok,R::AlreadyCached,R::TooLarge,R::Oom,R::InvalidFont}) {
    for(bool accepted : {false,true}) {
      family.vector=vector;
      settings.sdFontFlashPreload=1;
      sdFontSystem={};
      SdCardFontCache::next=check;
      TextSettingsActivity activity;
      activity.finishFinalFont(accepted);
      bool valid=check==R::Ok || check==R::AlreadyCached;
      bool prompt=check==R::Ok && !accepted;
      // An oversized .cpfont leaves quietly; an oversized vector font says why.
      bool tooLargeNotice=check==R::TooLarge && vector;
      bool complete=(check==R::TooLarge && !vector) || (valid && !prompt);
      assert(activity.attempts==int(check==R::Ok && accepted));
      assert(activity.prompts==int(prompt));
      assert(activity.errors==int((!valid && check!=R::TooLarge) || tooLargeNotice));
      assert(activity.completions==int(complete));
      assert(settings.sdFontFlashPreload==int(valid && !prompt));
      assert(sdFontSystem.releases==int(valid && !prompt));
      // Switching to the cached source must also drop a loaded vector face.
      assert(sdFontSystem.vectorReleases==sdFontSystem.releases);
    }
  }
}
''', defines=('RICKYOS_PRODUCT',))

    def test_regular_and_real_bold(self):
        result = pack.validate_cpfont(fixture(), {65})
        self.assertEqual([style["style"] for style in result], [0, 1])

    def test_truncated_data(self):
        with self.assertRaises(ValueError):
            pack.validate_cpfont(fixture()[:-1])

    def test_old_version_rejected(self):
        data = fixture()
        struct.pack_into("<H", data, 8, 3)
        with self.assertRaises(ValueError):
            pack.validate_cpfont(data)

    def test_required_chinese_glyph_is_not_silently_missing(self):
        with self.assertRaises(ValueError):
            pack.validate_cpfont(fixture(), {ord("阅")})

    def test_style_role_corruption(self):
        data = fixture()
        data[64] = 0
        with self.assertRaises(ValueError):
            pack.validate_cpfont(data)

    def test_glyph_bitmap_out_of_bounds(self):
        data = fixture()
        struct.pack_into("<I", data, 96 + 12 + 12, 200)
        with self.assertRaises(ValueError):
            pack.validate_cpfont(data)

    def test_interval_index_corruption(self):
        data = fixture()
        struct.pack_into("<I", data, 96 + 8, 10)
        with self.assertRaises(ValueError):
            pack.validate_cpfont(data)

    def test_wrong_source_stops_before_output_creation(self):
        with tempfile.TemporaryDirectory(prefix="ricky-font-pack-") as directory:
            sources = Path(directory) / "sources"
            sources.mkdir()
            (sources / pack.SOURCES[0]["regular"]).write_bytes(b"not an official font")
            output = Path(directory) / "output"
            with self.assertRaises(ValueError):
                pack.build(sources, output)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
