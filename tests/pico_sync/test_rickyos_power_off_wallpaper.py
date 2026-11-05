"""Compile the real wallpaper installer/options against a fault-injecting SD/settings seam."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class PowerOffWallpaperTest(unittest.TestCase):
    def test_independent_install_routing_and_rollback(self):
        viewer = (ROOT / 'src/activities/util/ImageViewerActivity.cpp').read_text()
        constants = viewer[viewer.index('constexpr const char* TRANSPARENT_PREVIEW_PATH'):viewer.index('// Converted previews')]
        methods = '\n'.join(method(viewer, signature) for signature in (
            'bool ImageViewerActivity::doSetSleepCover(',
            'void ImageViewerActivity::showSleepCoverOptions()'))
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
''' + constants + r'''
struct StorageMock;
struct HalFile {
  std::string* bytes = nullptr;
  size_t offset = 0;
  bool output = false;
  uint64_t fileSize64() const { return bytes->size(); }
  void flush() {}
  int read(char*, size_t);
  int write(const char*, size_t);
};
struct StorageMock {
  std::map<std::string, std::string> data;
  bool openFails = false, readFails = false, writeFails = false;
  std::string failRemove, failRename;
  bool exists(const char* p) { return data.contains(p); }
  bool remove(const char* p) {
    if (failRemove == p) return false;
    return data.erase(p) != 0;
  }
  bool rename(const char* a, const char* b) {
    if (failRename == a || !exists(a) || exists(b)) return false;
    data[b] = data[a]; data.erase(a); return true;
  }
  bool openFileForRead(const char*, const char* p, HalFile& f) {
    if (openFails || !exists(p)) return false;
    f.bytes = &data[p]; return true;
  }
  bool openFileForWrite(const char*, const char* p, HalFile& f) {
    if (openFails) return false;
    data[p].clear(); f.bytes = &data[p]; f.output = true; return true;
  }
} Storage;
int HalFile::read(char* out, size_t size) {
  if (Storage.readFails) return -1;
  size = std::min(size, bytes->size() - offset);
  memcpy(out, bytes->data() + offset, size); offset += size; return static_cast<int>(size);
}
int HalFile::write(const char* in, size_t size) {
  if (Storage.writeFails) return 0;
  bytes->append(in, size); return static_cast<int>(size);
}
struct CrossPointSettings {
  enum SLEEP_SCREEN_MODE { CUSTOM = 2, TRANSPARENT = 7 };
  enum { RICKY_STANDBY_PICTURE = 0, RICKY_POWER_OFF_CUSTOM = 3 };
  enum { READING_BACKGROUND_OFF = 0, READING_BACKGROUND_MEDIUM = 2, READING_PAPER_PICTURE = 0 };
  uint8_t sleepScreen = 6, rickyStandbyFace = 4, rickyPowerOffScreen = 1;
  uint8_t readingBackground = 0, readingPaper = 2;
  int failedSaves = 0;
  bool saveToFile() { if (failedSaves) { --failedSaves; return false; } return true; }
} SETTINGS;
struct GfxRenderer { struct FrameBufferLoan { explicit FrameBufferLoan(GfxRenderer&) {} }; };
struct PngToBmpConverter {
  static bool pngFileToTransparentBmpFile(const char*, const char* out, bool) {
    Storage.data[out] = "alpha"; return true;
  }
};
enum class StrId { STR_NORMAL, STR_TRANSPARENT, STR_RICKY_SET_WALLPAPER, STR_RICKY_SET_POWER_OFF_WALLPAPER };
constexpr int STR_LOADING_POPUP = 0, STR_DONE = 1, STR_FAILED_LOWER = 2;
const char* tr(int) { return ""; }
struct { void drawPopup(GfxRenderer&, const char*) {} } GUI;
void delay(int) {}
constexpr StrId sleepCoverLabel() { return StrId::STR_NORMAL; }
namespace FsHelpers {
bool hasJpgExtension(const std::string& p) { return p.ends_with(".jpg") || p.ends_with(".jpeg"); }
}
struct Popup {
  template<class F> void show(StrId, const StrId*, int, int, F cb) { cb(0); }
};
namespace ReadingBackground {
constexpr char kPath[] = "/readingbg.bmp";
int invalidations = 0;
void invalidate() { ++invalidations; }
}
struct ImageViewerActivity {
  enum class WallpaperTarget : uint8_t { Standby, PowerOff, Reading, Sticker };
  WallpaperTarget wallpaperTarget = WallpaperTarget::Standby;
  bool imageReady = true, wallpaperPicker = true, stickerPicker = false;
  bool png = false, jpeg = false;
  std::string filePath = "/original.bmp", previewPath = "/converted.bmp";
  int finishes = 0, stickers = 0;
  GfxRenderer renderer; Popup sleepCoverPopup;
  bool isPng() const { return png; }
  bool isJpeg() const { return jpeg; }
  bool doSetSticker() { ++stickers; return true; }
  void finish() { ++finishes; }
  void requestUpdate() {}
  bool gallery = false;
  const char* wallpaperSource() const { return png || jpeg ? previewPath.c_str() : filePath.c_str(); }
  bool setFromGallery(WallpaperTarget) { return true; }
  bool setStickerFromGallery() { ++stickers; return true; }
  void rememberSource() const {}
  bool doSetSleepCover(const char*, bool);
  void showSleepCoverOptions();
};
constexpr StrId sleepCoverLabel(bool, ImageViewerActivity::WallpaperTarget) { return StrId::STR_NORMAL; }
''' + methods + r'''
void reset() {
  Storage = {}; SETTINGS = {};
  Storage.data = {{"/sleep.bmp", "standby"}, {"/poweroff.bmp", "old-off"},
                  {"/original.bmp", std::string(10000, 'N')}, {"/converted.bmp", "converted"}};
}
void preserved() {
  assert(Storage.data.at("/sleep.bmp") == "standby");
  assert(SETTINGS.sleepScreen == 6 && SETTINGS.rickyStandbyFace == 4);
  assert(Storage.data.at("/original.bmp") == std::string(10000, 'N'));
}
int main() {
  using Target = ImageViewerActivity::WallpaperTarget;
  ImageViewerActivity off; off.wallpaperTarget = Target::PowerOff;
  reset(); assert(off.doSetSleepCover("/original.bmp", false)); preserved();
  assert(Storage.data.at("/poweroff.bmp") == Storage.data.at("/original.bmp"));
  assert(SETTINGS.rickyPowerOffScreen == 3);
  assert(!Storage.exists(POWER_OFF_IMAGE_PART_PATH) && !Storage.exists(POWER_OFF_IMAGE_BACKUP_PATH));
  // Changing standby afterwards leaves both the power-off image and its selection alone.
  ImageViewerActivity standby;
  assert(standby.doSetSleepCover("/converted.bmp", false));
  assert(Storage.data.at("/poweroff.bmp") == Storage.data.at("/original.bmp"));
  assert(SETTINGS.rickyPowerOffScreen == 3 && SETTINGS.rickyStandbyFace == 0 && SETTINGS.sleepScreen == 2);
  // BMP uses the original; JPEG/PNG use only the successfully prepared BMP.
  for (const char* ext : {"bmp", "png", "jpg", "jpeg"}) {
    reset(); off.filePath = std::string("/image.") + ext;
    off.png = std::string(ext) == "png";
    off.jpeg = std::string(ext) == "jpg" || std::string(ext) == "jpeg";
    Storage.data[off.filePath] = "original"; off.finishes = off.stickers = 0;
    off.showSleepCoverOptions();
    assert(off.finishes == 1 && off.stickers == 0);
    assert(Storage.data.at("/poweroff.bmp") == (std::string(ext) == "bmp" ? "original" : "converted"));
    assert(Storage.data.at(off.filePath) == "original"); preserved();
  }
  // The detected format, not the name, must choose the decoded BMP for power-off.
  reset(); off.filePath = "/jpeg-saved-as.png"; off.png = false; off.jpeg = true;
  Storage.data[off.filePath] = "jpeg-source";
  off.showSleepCoverOptions();
  assert(Storage.data.at("/poweroff.bmp") == "converted");
  assert(Storage.data.at(off.filePath) == "jpeg-source"); preserved();
  reset(); off.filePath = "/png-saved-as.jpg"; off.png = true; off.jpeg = false;
  Storage.data[off.filePath] = "png-source";
  off.showSleepCoverOptions();
  assert(Storage.data.at("/poweroff.bmp") == "converted");
  assert(Storage.data.at(off.filePath) == "png-source"); preserved();
  reset(); off.imageReady = false; off.finishes = 0; off.showSleepCoverOptions();
  assert(off.finishes == 0 && Storage.data.at("/poweroff.bmp") == "old-off");
  off.imageReady = true;
  assert(!off.doSetSleepCover("/original.bmp", true));  // no transparent-page/sticker path on power-off
  for (int fault = 0; fault < 7; ++fault) {
    reset();
    if (fault == 0) Storage.openFails = true;
    if (fault == 1) Storage.readFails = true;
    if (fault == 2) Storage.writeFails = true;
    if (fault == 3) Storage.failRename = "/poweroff.bmp";
    if (fault == 4) Storage.failRename = "/poweroff.bmp.part";
    if (fault == 5) SETTINGS.failedSaves = 1;
    if (fault == 6) { Storage.data["/poweroff.bmp.part"] = "stale"; Storage.failRemove = "/poweroff.bmp.part"; }
    assert(!off.doSetSleepCover("/original.bmp", false)); preserved();
    assert(Storage.data.at("/poweroff.bmp") == "old-off" && SETTINGS.rickyPowerOffScreen == 1);
  }
  reset(); Storage.data["/original.bmp"].clear();
  assert(!off.doSetSleepCover("/original.bmp", false));
  assert(Storage.data.at("/poweroff.bmp") == "old-off");
  // No old picture + settings failure: do not leave an unselected replacement behind.
  reset(); Storage.data.erase("/poweroff.bmp"); SETTINGS.failedSaves = 1;
  assert(!off.doSetSleepCover("/original.bmp", false)); assert(!Storage.exists(POWER_OFF_IMAGE_PATH)); preserved();
  // Interrupted old->backup rename: recover the previous complete image before another attempt.
  reset(); Storage.rename(POWER_OFF_IMAGE_PATH, POWER_OFF_IMAGE_BACKUP_PATH); Storage.readFails = true;
  assert(!off.doSetSleepCover("/original.bmp", false));
  assert(Storage.data.at("/poweroff.bmp") == "old-off"); preserved();
  // 阅读背景: its own file; the picture paper is chosen and shown (medium when it was off).
  ImageViewerActivity reading; reading.wallpaperTarget = Target::Reading;
  reset(); ReadingBackground::invalidations = 0;
  assert(reading.doSetSleepCover("/original.bmp", false)); preserved();
  assert(Storage.data.at("/readingbg.bmp") == Storage.data.at("/original.bmp"));
  assert(Storage.data.at("/poweroff.bmp") == "old-off" && SETTINGS.rickyPowerOffScreen == 1);
  assert(SETTINGS.readingPaper == 0 && SETTINGS.readingBackground == 2 && ReadingBackground::invalidations == 1);
  assert(!Storage.exists("/readingbg.bmp.part") && !Storage.exists("/readingbg.bmp.bak"));
  reset(); SETTINGS.readingBackground = 3;
  assert(reading.doSetSleepCover("/original.bmp", false) && SETTINGS.readingBackground == 3);  // a set shade stays
  assert(!reading.doSetSleepCover("/original.bmp", true));  // no transparent path for the reading background
  // A failed save restores the paper and the shade, and leaves no unselected picture.
  reset(); ReadingBackground::invalidations = 0; SETTINGS.failedSaves = 1;
  assert(!reading.doSetSleepCover("/original.bmp", false)); preserved();
  assert(!Storage.exists("/readingbg.bmp") && SETTINGS.readingPaper == 2 && SETTINGS.readingBackground == 0);
  assert(ReadingBackground::invalidations == 0);
}
''', defines=('RICKYOS_PRODUCT',))

    def test_picker_cancel_keeps_target_and_saved_ids(self):
        page = (ROOT / 'src/activities/apps/standby/RickyStandbySettingsActivity.cpp').read_text()
        # Power-off pictures and transparent wallpapers both come from the gallery grid.
        self.assertNotIn('openStickerPicker', page)
        activate = method(page, 'void RickyStandbySettingsActivity::activateIndex(')
        self.assertIn('openGallery(StandbyPictures::Target::PowerOff);', activate)
        self.assertIn('openGallery(StandbyPictures::Target::Sticker);', activate)
        gallery = (ROOT / 'src/activities/apps/standby/StandbyGalleryActivity.cpp').read_text()
        self.assertIn('ImageViewerActivity::WallpaperTarget::PowerOff', gallery)
        settings = (ROOT / 'src/CrossPointSettings.h').read_text()
        enum = settings.split('enum RICKY_POWER_OFF_SCREEN : uint8_t {')[1].split('};')[0]
        positions = [enum.index(key) for key in ('RICKY_POWER_OFF_SAME = 0', 'RICKY_POWER_OFF_DEFAULT',
                                                'RICKY_POWER_OFF_BLANK', 'RICKY_POWER_OFF_CUSTOM',
                                                'RICKY_POWER_OFF_SCREEN_COUNT')]
        self.assertEqual(positions, sorted(positions))


if __name__ == '__main__':
    unittest.main()
