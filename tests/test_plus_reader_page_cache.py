"""Execute production native cache and next-chapter paths with the upstream host seams."""
from pathlib import Path
import importlib.util
import sys

root=Path('firmware/rickyos-plus-1.2/upstream').resolve()
sys.path.insert(0,str(root/'scripts/tests'))
spec=importlib.util.spec_from_file_location('reader_cache_seams',root/'scripts/tests/test_reader_page_cache.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
captured=[]
original_run=module.run_cpp
module.run_cpp=lambda program,include_dirs=():captured.append((program,include_dirs))
module.ReaderPageCacheTest().test_cache_identity_cancellation_and_lifetime()
program,includes=captured[0]
program=program[:program.index('void reset()')]
program='#define FREEINK_DEVICE_METALIO_PLUS 1\n'+program
program=program.replace('uint8_t textAntiAliasing=1,', 'uint8_t nativePageTurn=0; uint8_t textAntiAliasing=1,')
# 阅读背景 draws nothing here: these paths are about cache identity, not the paper.
program=program.replace('struct GfxRenderer;', '''struct GfxRenderer;
namespace ReadingBackground {
struct TextBox {int left=0,top=0,right=0,bottom=0,pitch=0,rule=0;};
template<class Renderer> bool paint(Renderer&,const TextBox&){return false;}
}''',1)
program=program.replace('struct GfxRenderer;', '''
int nativeImageBlocks=0, cancelNativeImageAt=0;
bool failNativeImage=false;
void nativeImageStep();
std::vector<int> pageLoads;
struct GfxRenderer;
''')
program=program.replace('    *error=cancel.isCancelled()?ImageRenderError::Cancelled:ImageRenderError::None;', '''
    if(failNativeImage){*error=ImageRenderError::Failed;return false;}
    for(int block=0;block<8;++block){
      if(cancel.isCancelled()){*error=ImageRenderError::Cancelled;return false;}
      nativeImageStep();
    }
    *error=cancel.isCancelled()?ImageRenderError::Cancelled:ImageRenderError::None;
''')
program=program.replace('    ++loads;', '    ++loads;pageLoads.push_back(index);')
program=program.replace('p->images=images;', '''p->images=images;
    if(images){p->elements.clear();p->elements.push_back(std::make_unique<PageImage>());}
''')
program=program.replace('  bool inverted=false;', '  bool inverted=false; int grayLevels=16; uint8_t* active16=nullptr;\n  int getGrayscaleLevels() const {return grayLevels;}')
program=program.replace('bool supportsTextOnlyCombinedBase() const {return true;}', 'bool supportsTextOnlyCombinedBase() const {return false;}')
program=program.replace('size_t grayscale16FrameBytes() const {return 0;}', 'size_t grayscale16FrameBytes() const {return 64;}')
program=program.replace('bool beginGrayscale16Offscreen(uint8_t*) {return false;}', 'bool beginGrayscale16Offscreen(uint8_t* p) {active16=p;memset(p,0xff,64);frame.fill(0xff);mode=BW;return true;}')
program=program.replace('void endGrayscale16Offscreen() {}', 'void endGrayscale16Offscreen() {active16=nullptr;}')
program=program.replace('    renderer.fonts.missing=missing;', '    if(renderer.active16)renderer.active16[yPos]=static_cast<uint8_t>(0x30+yPos);\n    renderer.fonts.missing=missing;')
program=program.replace('struct Section {', '''struct Epub {
 size_t chapterBytes=100;
 int getSpineItemsCount() const {return 9;}
 size_t getCumulativeSpineItemSize(int i)const{return i*chapterBytes;}
};
template<class T,class... Args> auto makeUniqueNoThrow(Args&&... args){return std::unique_ptr<T>(new(std::nothrow)T(std::forward<Args>(args)...));}
unsigned heapFree=1<<20;
constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP=32768, BACKGROUND_BUILD_MIN_MAX_ALLOC=32768;
struct Esp {size_t getFreeHeap(){return heapFree;}size_t getMaxAllocHeap(){return heapFree;}} ESP;
bool nextChapterImages=false;
struct Section {
 Section()=default;Section(std::shared_ptr<Epub>,int,GfxRenderer&):images(nextChapterImages){}
 bool partial=false;
 bool isPartial()const{return partial;}
 bool loadSectionFile(const ReaderRenderSpec&){return true;}
 bool createSectionFile(const ReaderRenderSpec&){return true;}
''')
program=program.replace('  bool nativeGray16Page(const Page&) const {return false;}', '  bool nativeGray16Page(const Page&) const {return true;}\n  std::shared_ptr<Epub> epub=std::make_shared<Epub>();')
program=program.replace('  void freePageCache();','  void freePageCache();\n  void prefetchNextChapter(const ReaderPageCacheKey&,uint32_t);')
source=module.READER.read_text()
program+='\n'+module.method(source,'void EpubReaderActivity::prefetchNextChapter(')
program+=r'''
void nativeImageStep(){if(++nativeImageBlocks==cancelNativeImageAt)activityManager.cancelled=true;}
void resetNative(){assert(memory::live==0);memory::allocations=0;memory::failAt=0;memory::headroom=true;activityManager={};SETTINGS={};renders=0;cancelAt=0;heapFree=1<<20;nativeImageBlocks=0;cancelNativeImageAt=0;failNativeImage=false;nextChapterImages=false;pageLoads.clear();}
int main(){
 for(int style=1;style<=3;++style){
  resetNative();SETTINGS.textAntiAliasing=0;SETTINGS.nativePageTurn=style;
  EpubReaderActivity r;const auto original=r.renderer.frame;
  assert(r.pageCacheEligible());r.renderIdle(0);
  assert(memory::live==6 && memory::allocations==6 && r.renderer.frame==original);
  assert(!r.pageCache_[0].key.antiAliasing && r.pageCacheIs16_[0]);
  assert(r.consume());
  const auto key=r.pageCache_[0].key;SETTINGS.textAntiAliasing=1;
  assert(!(key==r.pageCacheKey(key.page,1,1,1,1)));
  r.freePageCache();assert(memory::live==0);
 }
 for(int style:{0,4}){
  resetNative();SETTINGS.textAntiAliasing=0;SETTINGS.nativePageTurn=style;
  EpubReaderActivity r;assert(!r.pageCacheEligible());r.renderIdle(0);assert(memory::allocations==0);
 }
 for(int fail=1;fail<=6;++fail){
  resetNative();SETTINGS.textAntiAliasing=0;SETTINGS.nativePageTurn=3;memory::failAt=fail;
  EpubReaderActivity r;const auto original=r.renderer.frame;r.renderIdle(0);
  assert(r.renderer.frame==original && r.renderer.active16==nullptr && memory::live<=6);
  int owned=0;for(int slot=0;slot<2;++slot){
   owned+=bool(r.pageCacheBase_[slot])+bool(r.pageCacheStash_[slot])+bool(r.pageCache16_[slot]);
   if(r.pageCache_[slot].state==ReaderPageCache::State::Ready)
    assert(r.pageCacheBase_[slot] && r.pageCacheStash_[slot] && r.pageCache16_[slot]);
  }
  assert(memory::live==owned);r.freePageCache();assert(memory::live==0);
 }
 resetNative();
 {
  EpubReaderActivity r;const auto original=r.renderer.frame;
  assert(r.pageCacheEligible());r.renderIdle(0);
  assert(memory::live==6 && memory::allocations==6);
  assert(r.renderer.frame==original && r.renderer.active16==nullptr);
  for(int i=0;i<2;++i){
   assert(r.pageCacheIs16_[i] && r.pageCache_[i].state==ReaderPageCache::State::Ready);
   assert(!r.pageCacheLsb_[i] && !r.pageCacheMsb_[i]);
   assert(r.pageCache16_[i].get()[0]==0x30 && r.pageCache16_[i].get()[2]==0x32);
  }
  r.renderIdle(0);assert(memory::allocations==6);
  assert(r.consume() && r.pageCacheLiveSlot_==0 && r.missingGlyph(true)==0x4e05);
  SETTINGS.spec.verticalText=true;assert(!r.consume());
  r.freePageCache();assert(memory::live==0);
 }
 resetNative();
 {
  EpubReaderActivity r;const auto original=r.renderer.frame;cancelAt=1;r.renderIdle(0);
  assert(r.pageCache_[0].state==ReaderPageCache::State::Empty);
  assert(r.renderer.frame==original && r.renderer.active16==nullptr);
  activityManager.cancelled=false;cancelAt=0;r.renderIdle(0);
  assert(r.pageCache_[0].state==ReaderPageCache::State::Ready);
 }
 for(int fail=1;fail<=6;++fail){
  resetNative();EpubReaderActivity r;const auto original=r.renderer.frame;memory::failAt=fail;r.renderIdle(0);
  assert(r.renderer.frame==original && r.renderer.active16==nullptr && memory::live<=6);
  r.freePageCache();assert(memory::live==0);
 }
 resetNative();
 {EpubReaderActivity r;memory::headroom=false;r.renderIdle(0);assert(r.pageCacheFailed_ && memory::live==0);}
 resetNative();
 {EpubReaderActivity r;r.renderer.grayLevels=4;assert(!r.pageCacheEligible());r.renderIdle(0);assert(memory::live==0);}
 resetNative();
 {EpubReaderActivity r;r.renderer.inverted=true;assert(!r.pageCacheEligible());}
 resetNative();
 {
  EpubReaderActivity r;const auto original=r.renderer.frame;r.section->images=true;cancelNativeImageAt=3;
  r.renderIdle(0);
  assert(nativeImageBlocks==3 && r.pageCache_[0].state==ReaderPageCache::State::Empty);
  assert(r.renderer.frame==original && r.renderer.active16==nullptr && !r.pageCacheFailed_);
  activityManager.cancelled=false;cancelNativeImageAt=0;nativeImageBlocks=0;r.renderIdle(0);
  assert(nativeImageBlocks==16 && r.pageCache_[0].state==ReaderPageCache::State::Ready);
  assert(r.pageCache_[1].state==ReaderPageCache::State::Ready && memory::live==6);
 }
 resetNative();
 {
  EpubReaderActivity r;r.section->images=true;failNativeImage=true;r.renderIdle(0);
  assert(r.pageCache_[0].state==ReaderPageCache::State::Ready); // Existing failure placeholder is retained.
 }
 resetNative();
 {
  EpubReaderActivity r;r.section->currentPage=19;r.renderedPageKey_=r.pageCacheKey(19,1,1,1,1);r.renderIdle(0);
  assert(pageLoads.size()==2 && pageLoads[0]==0 && pageLoads[1]==18); // Forward chapter before backward page.
  assert(r.pageCache_[0].state==ReaderPageCache::State::Ready);
  assert(r.pageCache_[0].key.spine==3 && r.pageCache_[0].key.page==0 && r.pageCache_[0].key.sectionGeneration==2);
  ++r.currentSpineIndex;++r.sectionGeneration_;r.section->currentPage=-1;
  assert(r.consume() && r.pageCacheLiveSlot_==0);
 }
 for(int reason=0;reason<3;++reason){
  resetNative();EpubReaderActivity r;r.section->currentPage=19;r.renderedPageKey_=r.pageCacheKey(19,1,1,1,1);
  if(reason==0)r.epub->chapterBytes=24*1024+1;
  if(reason==1)heapFree=0;
  if(reason==2)r.section->partial=true;
  r.renderIdle(0);assert(r.pageCache_[0].state==ReaderPageCache::State::Empty);
 }
 resetNative();
 {
  EpubReaderActivity r;const auto original=r.renderer.frame;
  r.section->currentPage=19;r.renderedPageKey_=r.pageCacheKey(19,1,1,1,1);
  nextChapterImages=true;cancelNativeImageAt=3;r.renderIdle(0);
  assert(nativeImageBlocks==3 && r.pageCache_[0].state==ReaderPageCache::State::Empty);
  assert(r.renderer.frame==original && r.renderer.active16==nullptr);
  activityManager.cancelled=false;cancelNativeImageAt=0;nativeImageBlocks=0;r.renderIdle(0);
  assert(nativeImageBlocks==8 && r.pageCache_[0].state==ReaderPageCache::State::Ready);
  assert(r.pageCache_[0].key.spine==3 && r.pageCache_[0].key.page==0);
 }
}
'''
original_run(program,includes)
print('Plus native reader cache: forward/back slots, native pixels/proxy restoration, six bounded PSRAM buffers, cancellation/retry, OOM cleanup, format gate and bounded next-chapter identity passed')
