"""Run the production full JPEG and streaming PNG decoders under ASan/UBSan.

Synthetic progressive fixture is a 32x32 two-pixel checker, not user book art.
No Pillow or other image package is required for regression runs.
"""
from pathlib import Path
import base64
import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

PORT = Path(__file__).resolve().parents[1]
APP = PORT / 'firmware/rickyos-plus-1.2'
SRC = APP / 'upstream/lib/Epub/Epub/converters'
DEPS = PORT / '.local/dependencies/PNGdec/src'
JPEG = PORT / '.local/dependencies/JPEGDEC/src'
PROGRESSIVE = '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAEBAQEBAQEBAQEBAQECAgMCAgICAgQDAwIDBQQFBQUEBAQFBgcGBQUHBgQEBgkGBwgICAgIBQYJCgkICgcICAj/wgALCAAgACABAREA/8QAFQABAQAAAAAAAAAAAAAAAAAAAAL/2gAIAQEAAAABkAAf/8QAFRABAQAAAAAAAAAAAAAAAAAAADb/2gAIAQEAAQUCm02m02m02m02m02m02m02m02/8QAHxAAAQEJAQAAAAAAAAAAAAAAFBEAEhUWF0FXYYWR/9oACAEBAAY/AsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2RsbS30KaE+xQ5djv2Rv/EABQQAQAAAAAAAAAAAAAAAAAAAED/2gAIAQEAAT8hA6OXRy6OXRy6OXRy6OXRy6OXRy6OXRy6OXRy6OXRz//aAAgBAQAAABAAAP/EABUQAQEAAAAAAAAAAAAAAAAAAPDx/9oACAEBAAE/EBMzITMyEzMhMzITMyEzMhMzITMyEzMhMzITMyEzMhMzITMyEzMhMzL/2Q=='

STUBS = {
'Logging.h': '#pragma once\n#define LOG_DBG(...) ((void)0)\n#define LOG_INF(...) ((void)0)\n#define LOG_ERR(...) ((void)0)\n',
'BuildScratch.h': '#pragma once\n#include <cstddef>\nnamespace buildscratch {inline uint8_t* claim(size_t){return nullptr;} inline void release(void*){}}\n',
'FsHelpers.h': '#pragma once\n#include <string>\nnamespace FsHelpers {inline bool hasPngExtension(const std::string& s){return s==".png";}inline bool hasJpgExtension(const std::string& s){return s==".jpg"||s==".jpeg";}}\n',
'HalMemory.h': r'''#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
inline int allocs=0, failAt=0, live=0;inline size_t capacity=32*1024*1024;
class HalMemory {public:
struct HeapStats {size_t freeBytes,totalBytes,minFreeBytes,largestBlockBytes;};
struct PsramDeleter {void operator()(uint8_t* p)const{if(p){--live;free(p);}}};
using PsramBuffer=std::unique_ptr<uint8_t[],PsramDeleter>;
static PsramBuffer allocatePsram(size_t n){++allocs;if(allocs==failAt)return {};auto* p=static_cast<uint8_t*>(malloc(n));if(p)++live;return PsramBuffer(p);}
static HeapStats getPsramHeap(){return {capacity,capacity,capacity,capacity};}
static HeapStats getDefaultHeap(){return getPsramHeap();}
static HeapStats getInternalHeap(){return getPsramHeap();}
};
''',
'Memory.h': r'''#pragma once
#include <HalMemory.h>
#include <new>
template<class F> struct ScopedCleanup {F f;~ScopedCleanup(){f();}};
template<class T,class... A> auto makeUniqueNoThrow(A&&... a){return std::unique_ptr<T>(new(std::nothrow)T(std::forward<A>(a)...));}
template<class T> auto makeUniqueNoThrow(size_t n){return std::unique_ptr<T>(new(std::nothrow)std::remove_extent_t<T>[n]);}
namespace memory {
using ByteBuffer=HalMemory::PsramBuffer;
inline bool psramHasHeadroom(size_t n,size_t,size_t reserve){return n+reserve<=capacity;}
inline ByteBuffer makePsramByteBufferUninitializedNoThrow(size_t n){return HalMemory::allocatePsram(n);}
}
''',
'HalStorage.h': r'''#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
inline int readCalls=0,cancelReadAt=0;inline bool cancelled=false,shortRead=false;
struct HalFile {
std::shared_ptr<std::ifstream> f;size_t bytes=0,pos=0;
size_t size()const{return bytes;}size_t fileSize()const{return bytes;}size_t position()const{return pos;}
bool seek(size_t p){if(p>bytes)return false;f->clear();f->seekg(p);pos=p;return bool(*f);}
int read(void* out,size_t n){++readCalls;if(readCalls==cancelReadAt)cancelled=true;n=std::min(n,bytes-pos);if(shortRead&&n)--n;f->read(static_cast<char*>(out),n);int got=f->gcount();pos+=got;return got;}
void close(){f.reset();}bool isOpen()const{return bool(f);}
};
struct Store {bool openFileForRead(const char*,const std::string& path,HalFile& out){
out.f=std::make_shared<std::ifstream>(path,std::ios::binary);if(!*out.f)return false;
out.f->seekg(0,std::ios::end);out.bytes=out.f->tellg();return out.seek(0);}
} inline Storage;
''',
'GfxRenderer.h': r'''#pragma once
#include <cassert>
#include <cstdint>
#include <vector>
inline unsigned long millis(){return 0;}
struct GfxRenderer {
int width=128,height=128;bool active=true;
std::vector<uint8_t> pixels=std::vector<uint8_t>(128*128,255);
int getScreenWidth()const{return width;}int getScreenHeight()const{return height;}
bool isGrayscale16Active()const{return active;}
bool grayscale16PrefersColumns()const{return false;}
void drawGrayscale16Pixel(int x,int y,uint8_t gray){assert(x>=0&&x<width&&y>=0&&y<height);pixels[y*width+x]=gray;}
void blendGrayscale16Pixel(int x,int y,uint8_t gray,uint8_t alpha){assert(x>=0&&x<width&&y>=0&&y<height);auto& p=pixels[y*width+x];p=(gray*17*alpha+p*(15-alpha)+7)/15;}
};
''',
'DirectPixelWriter.h': r'''#pragma once
#include <GfxRenderer.h>
struct DirectPixelWriter {void init(GfxRenderer&){}void beginRow(int){}void writePixel(int,uint8_t,bool=false){assert(false);}};
struct DirectCacheWriter {void init(void*,int,int,int){}void beginRow(int,int){}void writePixel(int,uint8_t){assert(false);}};
''',
'PixelCache.h': r'''#pragma once
#include <string>
#include <CancelCheck.h>
struct PixelCache {uint8_t* buffer=nullptr;int bytesPerRow=0,bandRows=0,originX=0,bandStart=0;
bool begin(const std::string&,int,int,int,int,int){return false;}
bool advanceTo(int){return false;}void abort(){}bool finalize(CancelCheck={}){return false;}};
''',
}

MAIN = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include "GfxRenderer.h"
#include "HalMemory.h"
#include "PlusProgressiveJpeg.h"
#include "PngToFramebufferConverter.h"
#include "JpegToFramebufferConverter.h"
int yieldCalls=0,cancelYieldAt=0;
void ImageToFramebufferDecoder::yieldDuringDecode(uint32_t&){if(++yieldCalls==cancelYieldAt)cancelled=true;}
void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string&,const std::string&){}
bool ImageToFramebufferDecoder::validateAndStoreDimensions(int64_t w,int64_t h,ImageDimensions& d,const char*){
if(w<1||h<1||w>8192||h>8192||w*h>8388608)return false;d={static_cast<int16_t>(w),static_cast<int16_t>(h)};return true;}
void reset(){assert(live==0);allocs=failAt=readCalls=cancelReadAt=yieldCalls=cancelYieldAt=0;capacity=32*1024*1024;cancelled=shortRead=false;}
int main(int argc,char** argv){
assert(argc>=3);const std::string dir=argv[1];GfxRenderer r;RenderConfig cfg{};
cfg.x=cfg.y=3;cfg.maxWidth=cfg.maxHeight=32;cfg.useExactDimensions=true;cfg.output=DecodeOutput::NativeGrayscale16;cfg.bilinearScaling=true;
ImageRenderError error;cfg.error=&error;cfg.cancellation={nullptr,[](void*){return cancelled;}};
reset();assert(decodePlusProgressiveJpeg(dir+"/checker.jpg",r,cfg));assert(error==ImageRenderError::None&&live==0);
int worst=0;for(int y=0;y<32;++y)for(int x=0;x<32;++x){int expected=255*((x/2+y/2)%2);worst=std::max(worst,abs(int(r.pixels[(y+3)*128+x+3])-expected));}
assert(worst<12); // DC-only 1/8 preview cannot reproduce two-pixel alternating detail.
int totalAllocs=allocs;
int totalYields=yieldCalls;
for(int i=1;i<=totalYields;++i){reset();cancelYieldAt=i;assert(!decodePlusProgressiveJpeg(dir+"/checker.jpg",r,cfg)&&error==ImageRenderError::Cancelled&&live==0);}
for(int i=1;i<=totalAllocs;++i){reset();failAt=i;assert(!decodePlusProgressiveJpeg(dir+"/checker.jpg",r,cfg));assert(live==0);}
reset();capacity=1024;assert(!decodePlusProgressiveJpeg(dir+"/checker.jpg",r,cfg)&&error==ImageRenderError::OutOfMemory&&live==0);
reset();cancelled=true;assert(!decodePlusProgressiveJpeg(dir+"/checker.jpg",r,cfg)&&error==ImageRenderError::Cancelled&&allocs==0);
reset();cancelReadAt=2;assert(!decodePlusProgressiveJpeg(dir+"/checker.jpg",r,cfg)&&error==ImageRenderError::Cancelled&&live==0);
reset();shortRead=true;assert(!decodePlusProgressiveJpeg(dir+"/checker.jpg",r,cfg)&&live==0);
reset();assert(!decodePlusProgressiveJpeg(dir+"/oversized.jpg",r,cfg)&&live==0);
reset();r.active=false;assert(!decodePlusProgressiveJpeg(dir+"/checker.jpg",r,cfg)&&allocs==0);r.active=true;
// Actual converter routing: complete decode, recoverable PSRAM fallback, and
// cancellation. ASan also checks its JPEGDEC callback handle closes only once.
JpegToFramebufferConverter jpeg;
reset();assert(jpeg.decodeToFramebuffer(dir+"/checker.jpg",r,cfg)&&error==ImageRenderError::None&&live==0);
reset();failAt=1;assert(jpeg.decodeToFramebuffer(dir+"/checker.jpg",r,cfg)&&error==ImageRenderError::None&&live==0);
reset();cancelled=true;assert(!jpeg.decodeToFramebuffer(dir+"/checker.jpg",r,cfg)&&error==ImageRenderError::Cancelled&&live==0);
// Real PNGdec and the production two-row filter. A black RGB value at alpha=0
// must not darken the opaque white pixel beside it after interpolation.
PngToFramebufferConverter png;cfg.preserveAlpha=true;cfg.bilinearScaling=true;cfg.maxWidth=cfg.maxHeight=32;
for(const char* name:{"rgba","indexed","key"}){
 reset();std::fill(r.pixels.begin(),r.pixels.end(),102);
 assert(png.decodeToFramebuffer(dir+"/"+name+".png",r,cfg));assert(live==0);
 for(int y=0;y<32;++y){assert(r.pixels[(y+3)*128+3]==102);assert(r.pixels[(y+3)*128+34]==255);}
 assert(r.pixels[3*128+11]>102&&r.pixels[3*128+11]<255);
 assert(r.pixels[2*128+3]==102&&r.pixels[35*128+3]==102);
}
reset();cancelled=true;assert(!png.decodeToFramebuffer(dir+"/rgba.png",r,cfg)&&error==ImageRenderError::Cancelled&&live==0);
// Optional private sample: dump decoder output for independent libjpeg comparison.
if(argc==4){reset();r.width=684;r.height=1216;r.pixels.assign(r.width*r.height,255);cfg.x=cfg.y=0;cfg.maxWidth=600;cfg.maxHeight=1000;
 assert(decodePlusProgressiveJpeg(argv[3],r,cfg));std::ofstream f(argv[2],std::ios::binary);f.write(reinterpret_cast<char*>(r.pixels.data()),r.pixels.size());assert(live==0);}
}
'''


def run(sample=None, output=None):
    subprocess.run([sys.executable,str(PORT/'scripts/prepare_plus_jpeg.py')],check=True)
    assert hashlib.sha256((APP/'components/rickyos_application/port/vendor/stb_image.h').read_bytes()).hexdigest() == '63740a647de8e20bb2699d28829e8d482c90d3ddb0bf275c67380edff2a63be9'
    with tempfile.TemporaryDirectory(prefix='plus-image-quality-') as tmp:
        work = Path(tmp)
        for name, text in STUBS.items():
            (work/name).write_text(text)
        for name in ('PngToFramebufferConverter.cpp', 'PngToFramebufferConverter.h', 'JpegToFramebufferConverter.cpp', 'JpegToFramebufferConverter.h', 'ImageToFramebufferDecoder.h', 'DitherUtils.h','SampleRowClamp.h'):
            shutil.copyfile(SRC/name, work/name)
        (work/'main.cpp').write_text(MAIN)
        checker = base64.b64decode(PROGRESSIVE)
        (work/'checker.jpg').write_bytes(checker)
        big = bytearray(checker)
        sof = big.index(b'\xff\xc2')
        big[sof+5:sof+9] = struct.pack('>HH', 8192, 8192)
        (work/'oversized.jpg').write_bytes(big)
        def chunk(kind, data):
            return struct.pack('>I', len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
        for name, color, row in [('rgba',6,bytes([0,0,0,0,255,255,255,255])),('indexed',3,bytes([0,1])),('key',2,bytes([0,0,0,255,255,255]))]:
            data=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2,2,8,color,0,0,0))
            if color==3: data+=chunk(b'PLTE',bytes([0,0,0,255,255,255]))+chunk(b'tRNS',bytes([0,255]))
            if color==2: data+=chunk(b'tRNS',bytes(6))
            data+=chunk(b'IDAT',zlib.compress((b'\0'+row)*2))+chunk(b'IEND',b'')
            (work/(name+'.png')).write_bytes(data)
        flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
        includes=[work, APP/'components/rickyos_application/port',APP/'upstream/lib/Epub',APP/'upstream/lib/Memory',DEPS,JPEG]
        objects=[]
        for source in DEPS.glob('*.c'):
            obj=work/(source.stem+'.o');objects.append(obj)
            # Pinned PNGdec explicitly uses unaligned uint32 copies on ARM/x86.
            # Suppress only that known vendor check, not our converter/allocator.
            vendor_flags=['-fno-sanitize=alignment'] if source.name=='inflate.c' else []
            subprocess.run(['cc',*flags,*vendor_flags,'-I'+str(DEPS),'-c',str(source),'-o',str(obj)],check=True)
        jpeg_obj=work/'jpegdec.o';objects.append(jpeg_obj)
        # JPEGDEC's host optimized reader also uses intentional unaligned loads.
        subprocess.run(['c++','-std=c++20',*flags,'-fno-sanitize=alignment','-D__LINUX__=1',
                        '-I'+str(JPEG),'-c',str(PORT/'.local/jpegdec-plus/JPEGDEC.cpp'),'-o',str(jpeg_obj)],check=True)
        binary=work/'test'
        subprocess.run(['c++','-std=c++20',*flags,'-D__LINUX__=1','-DSIMULATOR=1','-DFREEINK_DEVICE_METALIO_PLUS=1','-DPNG_MAX_BUFFERED_PIXELS=16416',
                        *['-I'+str(p) for p in includes],str(work/'main.cpp'),str(work/'PngToFramebufferConverter.cpp'),
                        str(DEPS/'PNGdec.cpp'),str(work/'JpegToFramebufferConverter.cpp'),
                        str(APP/'components/rickyos_application/port/PlusProgressiveJpeg.cpp'),
                        *map(str,objects),'-o',str(binary)],check=True)
        args=[str(binary),str(work),str(output or work/'gray.raw')]
        if sample: args.append(str(sample))
        subprocess.run(args,check=True)
    print('Full progressive detail, per-allocation failure cleanup, PSRAM cap, cancellation, PNG premultiplied alpha/bilinear and page margins passed (ASan/UBSan)')


if __name__ == '__main__':
    import sys
    run(*sys.argv[1:])
