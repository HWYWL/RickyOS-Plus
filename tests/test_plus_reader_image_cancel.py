"""Execute the production image forwarding and bounded JPEG preload on host."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2/upstream'
sys.path.insert(0, str(root / 'scripts/tests'))
from test_reading_ui_regressions import method, run_cpp

jpeg = (root / 'lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp').read_text()
image = (root / 'lib/Epub/Epub/blocks/ImageBlock.cpp').read_text()
program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "CancelCheck.h"
namespace memory {
bool headroom=true, fail=false;
size_t attempts=0, live=0, reserve=0;
struct Free {void operator()(uint8_t* p) const {delete[] p;--live;}};
using ByteBuffer=std::unique_ptr<uint8_t,Free>;
bool psramHasHeadroom(size_t,size_t,size_t r){reserve=r;return headroom;}
ByteBuffer makePsramByteBufferUninitializedNoThrow(size_t bytes){
 ++attempts;if(fail)return {};++live;return ByteBuffer(new uint8_t[bytes]);
}
}
struct Stats {size_t reads=0,largest=0,closes=0;int cancelAt=0;bool cancelled=false,shortRead=false;} stats;
struct HalFile {
 size_t bytes=0,pos=0;bool opened=false;
 size_t fileSize()const{return bytes;}
 int read(uint8_t* out,size_t count){
  ++stats.reads;stats.largest=std::max(stats.largest,count);
  if(stats.cancelAt && stats.reads==static_cast<size_t>(stats.cancelAt))stats.cancelled=true;
  if(stats.shortRead)--count;
  memset(out,0x5a,count);pos+=count;return static_cast<int>(count);
 }
 ~HalFile(){if(opened)++stats.closes;}
};
struct StorageType {
 size_t bytes=100000;bool fail=false;
 bool openFileForRead(const char*,const std::string&,HalFile& f){
  if(fail)return false;f.bytes=bytes;f.opened=true;return true;
 }
} Storage;
''' + method(jpeg, '[[maybe_unused]] memory::ByteBuffer readJpegIntoPsram(') + r'''
enum class ImageRenderError {None,Cancelled};
enum class DecodeOutput {FrameBufferAndCache};
struct GfxRenderer {};
bool forwarded=false;
struct ImageBlock {
 enum class PixelCachePolicy {Stream};
 bool render(GfxRenderer&,int,int,PixelCachePolicy,ImageRenderError*,CancelCheck);
 bool renderInternal(GfxRenderer&,int x,int y,PixelCachePolicy,DecodeOutput out,
                     ImageRenderError* error,CancelCheck cancel){
  assert(x==2 && y==3 && out==DecodeOutput::FrameBufferAndCache);
  forwarded=true;*error=cancel.isCancelled()?ImageRenderError::Cancelled:ImageRenderError::None;
  return *error==ImageRenderError::None;
 }
};
''' + method(image, 'bool ImageBlock::render(') + r'''
void reset(){assert(memory::live==0);stats={};Storage={};memory::attempts=0;memory::headroom=true;memory::fail=false;}
int main(){
 size_t bytes=99;
 CancelCheck cancel{nullptr,[](void*){return stats.cancelled;}};
 reset();{
  auto data=readJpegIntoPsram("image",bytes,{});
  assert(data && bytes==100000 && data.get()[99999]==0x5a);
  assert(stats.reads==1 && stats.largest==100000 && stats.closes==1 && memory::reserve==256*1024);
 }
 reset();{
  auto data=readJpegIntoPsram("image",bytes,cancel);
  assert(data && bytes==100000 && stats.reads==7 && stats.largest==16*1024 && stats.closes==1);
 }
 for(int stop:{1,3,7}){
  reset();stats.cancelAt=stop;
  assert(!readJpegIntoPsram("image",bytes,cancel) && bytes==0);
  assert(stats.reads==static_cast<size_t>(stop) && stats.closes==1 && memory::live==0);
 }
 reset();stats.cancelled=true;
 assert(!readJpegIntoPsram("image",bytes,cancel) && stats.reads==0 && memory::attempts==0);
 for(int failure=0;failure<6;++failure){
  reset();
  if(failure==0)Storage.fail=true;
  if(failure==1)Storage.bytes=0;
  if(failure==2)Storage.bytes=4*1024*1024+1;
  if(failure==3)memory::headroom=false;
  if(failure==4)memory::fail=true;
  if(failure==5)stats.shortRead=true;
  assert(!readJpegIntoPsram("image",bytes,cancel) && bytes==0 && memory::live==0);
 }
 reset();GfxRenderer renderer;ImageBlock image;ImageRenderError error;
 stats.cancelled=true;
 assert(!image.render(renderer,2,3,ImageBlock::PixelCachePolicy::Stream,&error,cancel));
 assert(forwarded && error==ImageRenderError::Cancelled);
 stats.cancelled=false;
 assert(image.render(renderer,2,3,ImageBlock::PixelCachePolicy::Stream,&error,{}));
 assert(error==ImageRenderError::None);
}
'''
run_cpp(program, (root / 'lib/Memory',))
print('Plus image cancellation: production forwarding, bounded 16KiB idle reads, foreground bulk I/O, cancellation/OOM/short-read cleanup passed')
