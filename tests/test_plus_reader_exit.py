"""Run actual reader exit methods: wait before free, shed caches, clean destination."""
from pathlib import Path
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
APP = PORT / 'firmware/rickyos-plus-1.2/upstream'


def method(path, signature):
    source = path.read_text()
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


program = r'''
#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <vector>
std::vector<std::string> events;
#define LOG_INF(...) ((void)0)
struct FontCache {void releaseSdFontCaches(){events.push_back("font-free");}};
struct GfxRenderer {
 enum class Orientation {Portrait};
 FontCache cache;bool cacheAvailable=true,pending=false,borrowed=true;
 int fullRequests=0;
 FontCache* getFontCacheManager(){return cacheAvailable?&cache:nullptr;}
 void waitRefreshComplete(){borrowed=false;events.push_back("wait");}
 void setOrientation(Orientation){events.push_back("portrait");}
 void requestNextFullRefresh(){pending=true;++fullRequests;events.push_back("full");}
};
struct Activity {void onExit(){events.push_back("base-exit");}};
struct {int readerActivityLoadCount=4;void saveToFile(){events.push_back("save");}} APP_STATE;
struct {int warmups=99;void resetReaderWarmup(){warmups=0;events.push_back("warmup-reset");}} sdFontSystem;
struct Options {};
struct ReaderActivity:Activity {
 GfxRenderer renderer;
 std::unique_ptr<Options> endOfBookOptions=std::make_unique<Options>();
 std::atomic<bool> endOfBookOptionsReady{true};
 void flushReaderSession(){events.push_back("session-end");}
 void onExit();
};
struct {void endSession(){events.push_back("stats");}int getLastSessionSnapshot(){return 1;}} READING_STATS;
struct {void recordSessionEnded(int){events.push_back("achievements");}} ACHIEVEMENTS;
namespace reading_excerpt {void persist(){events.push_back("excerpt");}}
void showPendingAchievementPopups(GfxRenderer& r){assert(!r.pending);events.push_back("popup");}
namespace rickyBadgeNotice {void announceNew(GfxRenderer& r){assert(!r.pending);events.push_back("badge");}}
struct ImageBlock {static void releaseRenderCache(){events.push_back("image-free");}};
struct SavedPosition {int spineIndex=7,pageNumber=9;};
struct BookFont {~BookFont(){events.push_back("book-font-free");}};
struct EpubReaderActivity:ReaderActivity {
 int footnoteDepth=0;bool epub=true;SavedPosition savedPositions[1];
 std::unique_ptr<BookFont> bookFonts_=std::make_unique<BookFont>();
 bool overlayStored=true;
 void saveProgress(int spine,int page,int){assert(spine==7&&page==9);events.push_back("progress");}
 void settleOverlayRefresh(){assert(!renderer.borrowed);events.push_back("settle");}
 void discardOverlayPage(){assert(!renderer.borrowed);overlayStored=false;events.push_back("overlay-free");}
 void freePageCache(){
#if FREEINK_DEVICE_METALIO_PLUS
 assert(!renderer.borrowed);
#endif
 events.push_back("page-free");}
 void onExit();
};
''' + method(APP/'src/activities/reader/ReaderActivity.cpp', 'void ReaderActivity::onExit()') + '\n' + method(
    APP/'src/activities/reader/EpubReaderActivity.cpp', 'void EpubReaderActivity::onExit()') + r'''
size_t index(const char* e){for(size_t i=0;i<events.size();++i)if(events[i]==e)return i;return events.size();}
int main(){
 for(bool cache:{true,false})for(bool footnote:{true,false}){
  events.clear();sdFontSystem.warmups=99;
  EpubReaderActivity r;r.renderer.cacheAvailable=cache;r.footnoteDepth=footnote?1:0;r.onExit();
  assert(!r.bookFonts_&&!r.endOfBookOptions&&!r.endOfBookOptionsReady.load());
  assert(APP_STATE.readerActivityLoadCount==0);
#if FREEINK_DEVICE_METALIO_PLUS || FREEINK_DEVICE_READPICO
  assert(r.renderer.pending&&r.renderer.fullRequests==1);
  assert(events.back()=="full"); // Popups cannot consume the destination's cleanup.
#else
  assert(!r.renderer.pending);
#endif
#if FREEINK_DEVICE_METALIO_PLUS
  assert(!r.renderer.borrowed&&!r.overlayStored);
  assert(index("wait")<index("image-free")&&index("wait")<index("page-free"));
  assert(index("overlay-free")<index("page-free"));
  assert(sdFontSystem.warmups==0);
#endif
#if !defined(BOARD_HAS_PSRAM) || FREEINK_DEVICE_METALIO_PLUS
  assert((index("font-free")<events.size())==cache);
#else
  assert(index("font-free")==events.size()); // Other PSRAM boards retain policy.
#endif
  assert(index("book-font-free")<index("session-end"));
  assert((index("progress")<events.size())==footnote);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='plus-reader-exit-') as folder:
    p = Path(folder)
    (p/'test.cpp').write_text(program)
    for target, flags in [('plus', ['BOARD_HAS_PSRAM=1', 'FREEINK_DEVICE_METALIO_PLUS=1']),
                          ('pico', ['BOARD_HAS_PSRAM=1', 'FREEINK_DEVICE_READPICO=1']),
                          ('c3', []), ('other_psram', ['BOARD_HAS_PSRAM=1'])]:
        binary = p/target
        subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-DCROSSPOINT_VECTOR_FONTS=1', '-DRICKYOS_PRODUCT=1',
                        *('-D'+f for f in flags), str(p/'test.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print('Reader exit: actual Plus/Pico/C3/other-PSRAM paths, wait-before-free, RAM cache release, footnote progress, optional font cache and destination-only full cleanup passed (ASan/UBSan)')
