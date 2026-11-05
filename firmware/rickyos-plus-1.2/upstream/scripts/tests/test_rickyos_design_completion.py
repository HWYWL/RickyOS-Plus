"""Design gap fixes exercise real helpers and bounded storage ownership."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class DesignCompletionTest(unittest.TestCase):
    def test_middle_ellipsis_is_utf8_safe_keeps_suffix_and_does_not_allocate(self):
        run_cpp(r'''
#include <cassert>
#include <cstring>
#include <string>
#include "components/RickyFileLabel.h"
int width(const char* s) {
  int count=0;
  while(*s) { if(!RickyFileLabel::continuation(*s)) ++count; ++s; }
  return count;
}
bool valid(const char* s) {
  while(*s) {
    unsigned char c=*s++;
    int extra=c<0x80?0:(c&0xe0)==0xc0?1:(c&0xf0)==0xe0?2:(c&0xf8)==0xf0?3:-1;
    if(extra<0) return false;
    while(extra--) {if(!*s || !RickyFileLabel::continuation(*s++)) return false;}
  }
  return true;
}
int main() {
  for(const char* name : {"一本名字特别长的测试书籍（第三版）.epub",
                           "very-long-book-name-third-edition.epub", "没有扩展名的非常非常长的目录名称"}) {
    for(int budget=8;budget<30;++budget) {
      char copy[160]; std::strcpy(copy,name);
      RickyFileLabel::middleEllipsis(copy,budget,width);
      assert(valid(copy));
      assert(width(copy)<=budget);
      if(std::strstr(name,".epub") && budget>=12) assert(std::strstr(copy,".epub"));
    }
  }
  char shortName[]="短.epub";
  RickyFileLabel::middleEllipsis(shortName,20,width);
  assert(std::strcmp(shortName,"短.epub")==0);
}
''', include_dirs=(ROOT / 'src',))

    def test_metadata_never_scans_in_paint_and_is_bounded_in_loop(self):
        source = (ROOT / 'src/activities/home/FileBrowserActivity.cpp').read_text()
        for name in ('void FileBrowserActivity::buildScreen(', 'void FileBrowserActivity::provideRow('):
            body = method(source, name)
            self.assertNotIn('Storage.open', body)
            self.assertNotIn('openNextFile', body)
            self.assertNotIn('fileSize64', body)
        layout = method(source, 'void FileBrowserActivity::buildScreen(')
        self.assertIn('props.rowInset = screen.theme().listInset', layout)
        self.assertIn('props.sidePadding = screen.theme().listSidePadding', layout)
        self.assertIn('std::max<int>(0, props.rowInset) * 2', layout)
        scan = method(source, 'void FileBrowserActivity::updateMetadata(')
        self.assertIn('entries < 24', scan)
        self.assertIn('millis() - started < 3', scan)
        self.assertIn('entry.fileSize64()', scan)
        self.assertIn('RenderLock::Mode::Try', scan)
        self.assertIn('metadataDirectory_.close()', method(source, 'void FileBrowserActivity::onExit('))
        loop = method(source, 'void FileBrowserActivity::loop(')
        self.assertLess(loop.index('UiListActivity::loop()'), loop.index('updateMetadata()'))

    def test_menu_and_help_have_real_actions_not_decorative_entries(self):
        reader = (ROOT / 'src/activities/reader/EpubReaderMenuActivity.cpp').read_text()
        self.assertIn('pageBookmarked_ ? tr(STR_RICKY_MENU_REMOVE_BOOKMARK)', reader)
        # Rows draw through the shared design list: chevrons, the logical action id
        # past the quick cards, one-row toggles and the measured page.
        self.assertIn('style.chevron = true', reader)
        self.assertIn('menuView.quickCount);', reader)
        rows = (ROOT / 'src/components/RickyRowList.h').read_text()
        self.assertIn('setting.drawChevron = look.chevron', rows)
        self.assertIn('setting.valueId = static_cast<int16_t>(i + actionOffset)', rows)
        self.assertIn('one.nav = nullptr', rows)
        self.assertIn('nav.onListRendered(props.topIndex, drawn, selectedDrawn)', rows)
        settings = (ROOT / 'src/activities/settings/SettingsActivity.cpp').read_text()
        activate = method(settings, 'void SettingsActivity::activateIndex(')
        self.assertIn('index == categoryCount + 1', activate)
        self.assertIn('startActivityForResultWith<RickyHelpActivity>', activate)
        help_source = (ROOT / 'src/activities/settings/RickyHelpActivity.cpp').read_text()
        self.assertIn('startActivityForResultWith<QrDisplayActivity>', help_source)
        self.assertNotIn('WiFi', help_source)
        self.assertNotIn('HTTPClient', help_source)

    def test_metadata_slice_counts_only_direct_children_and_keeps_64_bit_sizes(self):
        source = (ROOT / 'src/activities/home/FileBrowserActivity.cpp').read_text()
        run_cpp(r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
unsigned long tick=500;
unsigned long millis() {return tick;}
int childReads=0;
struct HalFile {
  bool opened=false, directory=false;
  int position=0, child=-1;
  uint64_t bytes=0;
  explicit operator bool() const {return opened;}
  bool isDirectory() const {return directory;}
  uint64_t fileSize64() const {return bytes;}
  void close() {opened=false;}
  HalFile openNextFile() {
    ++childReads; ++tick;
    if(position>=101) return {};
    const int index=position++;
    return {true,false,0,index,123};
  }
  void getName(char* out,size_t size) {
    std::snprintf(out,size,"%s",child==0?".hidden":"item");
  }
};
struct StorageMock {
  HalFile open(const char* path) {
    if(std::strcmp(path,"/folder")==0) return {true,true};
    if(std::strcmp(path,"/large.txt")==0) return {true,false,0,-1,10ULL*1024*1024*1024};
    if(std::strcmp(path,"/zero.txt")==0) return {true,false};
    return {};
  }
} storage;
#define Storage storage
struct {bool showHiddenFiles=false;} settings;
#define SETTINGS settings
constexpr int NAME_BUFFER_SIZE=500;
constexpr const char* MOVE_HERE_ENTRY="\x01";
struct RenderLock {
  enum class Mode {Try};
  static inline bool allow=true;
  RenderLock(Mode) {} template<class T> RenderLock(T&) {}
  bool ownsLock() const {return allow;}
};
struct FileBrowserActivity {
  struct Metadata {uint64_t value=0;bool done=false,known=false;};
  static constexpr int METADATA_WINDOW=16;
  std::array<Metadata,16> metadata_{};
  std::atomic<int> requestedMetadataStart_{0};
  int metadataStart_=-1,metadataSlot_=-1,updates=0;
  uint64_t directoryCount_=0;
  HalFile metadataDirectory_;
  char metadataPath_[1024]{};
  bool metadataRepaint_=false;
  unsigned long metadataRepaintAt_=0;
  std::unique_ptr<char[]> fileNameBuffer=std::make_unique<char[]>(500);
  std::string basepath="/";
  std::vector<std::string> files{"folder/","large.txt","zero.txt"};
  void requestUpdate() {++updates;}
  void updateMetadata();
};
''' + method(source, 'void FileBrowserActivity::updateMetadata(') + r'''
int main() {
  FileBrowserActivity browser;
  browser.updateMetadata();
  assert(browser.metadata_[1].known && browser.metadata_[1].value==10ULL*1024*1024*1024);
  browser.updateMetadata();
  assert(browser.metadata_[2].known && browser.metadata_[2].value==0);
  assert(!browser.metadata_[0].known);
  for(int pass=0;pass<100;++pass) {
    int before=childReads;
    browser.updateMetadata();
    assert(childReads-before<=24);
  }
  assert(browser.metadata_[0].known && browser.metadata_[0].value==100);
  assert(!browser.metadataDirectory_ && browser.updates<10);
  browser.requestedMetadataStart_=1;
  browser.updateMetadata();
  assert(browser.metadataStart_==1);
  assert(browser.metadata_[0].value==10ULL*1024*1024*1024);
}
''')


if __name__ == '__main__':
    unittest.main()
