"""SD information is display-only; execute the production storage route dispatch."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "firmware/rickyos-plus-1.2/upstream/src/activities/home/RickyStorageActivity.cpp"
HEADER = SOURCE.with_suffix(".h")


class StorageSummaryTest(unittest.TestCase):
    def test_summary_has_no_touch_or_selection(self):
        source = SOURCE.read_text()
        summary = source[source.index("// SD card:"):source.index("// Three ways in:")]
        self.assertIn("constexpr bool summarySelected = false;", summary)
        self.assertNotIn(".hit(", summary)
        self.assertNotIn("ClearCacheActivity", source)

    def test_rows_and_refresh_keep_shared_navigation_indices(self):
        source = SOURCE.read_text()
        header = HEADER.read_text()
        self.assertIn("const int row = i;", source)
        self.assertIn("screen.frame().hit(rect, ACTION_ROW, row, fui::InputTouch)", source)
        self.assertIn("kCardCount = 3;", header)
        self.assertIn("kRefreshAction = kCardCount;", header)
        self.assertIn("return kCardCount + 1;", header)
        self.assertIn("contentEdgeIndex(edge, kCardCount)", header)
        self.assertIn("screen.frame().hit(hit, ACTION_ROW, kRefreshAction", source)

    def test_production_dispatch(self):
        source = SOURCE.read_text()
        count = re.search(r"kCardCount = (\d+);", HEADER.read_text()).group(1)
        dispatch = source[source.index("void RickyStorageActivity::activateIndex(int index)"):]
        code = r'''
#include <cassert>
#include <string>
#include <type_traits>
struct ActivityResult {};
struct FileBrowserActivity { enum class Mode { Books }; };
struct LibrarySourcesActivity {};
struct App { void clearTapFlash() {} } app;
struct Counts { bool stale = false; bool load() { return stale; } } countsStale;
struct Manager { int transfers = 0; void goToFileTransfer() { ++transfers; } } activityManager;
int scans = 0;
void startStorageScan(bool) { ++scans; }
struct RickyStorageActivity {
  static constexpr int kCardCount = COUNT;
  static constexpr int kRefreshAction = kCardCount;
  bool folderMissing = true;
  int files = 0, folders = 0, refreshes = 0, updates = 0;
  void refresh() { ++refreshes; }
  void countLibraryFolders() { ++folders; }
  void requestUpdate() { ++updates; }
  template<class T, class F, class... Args>
  void startActivityForResultWith(F callback, Args... args) {
    if constexpr (std::is_same_v<T, FileBrowserActivity>) {
      checkFile(args...); ++files;
    } else {
      static_assert(std::is_same_v<T, LibrarySourcesActivity>);
    }
    callback(ActivityResult{});
  }
  void checkFile(const char* path, FileBrowserActivity::Mode mode, bool scoped) {
    assert(std::string(path) == "/" && mode == FileBrowserActivity::Mode::Books && scoped);
  }
  void activateIndex(int index);
};
'''.replace("COUNT", count) + dispatch + r'''
int main() {
  RickyStorageActivity page;
  page.activateIndex(0);
  assert(page.files == 1 && page.folders == 0 && scans == 0 && !page.folderMissing);
  countsStale.stale = true;
  page.activateIndex(0);
  assert(page.files == 2 && scans == 1);
  page.activateIndex(1);
  assert(page.folders == 1 && page.updates == 1);
  page.activateIndex(2);
  assert(activityManager.transfers == 1);
  page.activateIndex(RickyStorageActivity::kRefreshAction);
  assert(page.refreshes == 1);
  page.activateIndex(-1); page.activateIndex(4);
  assert(page.files == 2 && page.folders == 1 && page.refreshes == 1 && activityManager.transfers == 1);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / "test.cpp").write_text(code)
            subprocess.run(["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-lambda-capture", "-fsanitize=address,undefined", str(path / "test.cpp"),
                            "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
