"""Run production section-header writer/validation with a bounded in-memory file."""
from pathlib import Path
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
EPUB = PORT / 'firmware/rickyos-plus-1.2/upstream/lib/Epub/Epub'
source = (EPUB / 'Section.cpp').read_text()
constants = source[source.index('// Keep separate cache-version'):source.index('// Called only between layout')]

def function(signature):
    a = source.index(signature)
    i = source.index('{', a) + 1
    depth = 1
    while depth:
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
        i += 1
    return source[a:i]

writer = function('void Section::writeSectionFileHeader(')
# The production prefix opens, reads and validates every layout key, before page
# LUT loading. Keep it verbatim so tests catch field ordering/truncated reads too.
reader = source[source.index('bool Section::loadSectionFile('):source.index('  if (!serialization::readPod(file, pageCount))')]
reader += '  (void)filePartial; return true;\n}\n'

PROGRAM = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "ReaderRenderSpec.h"
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
struct HalFile {
  std::vector<uint8_t> bytes; size_t position=0; bool open=true;
  explicit operator bool() const {return open;}
  void close() {open=false;}
};
namespace serialization {
template<class T> void writePod(HalFile& f,const T& pod) {
  const auto* p=reinterpret_cast<const uint8_t*>(&pod);
  f.bytes.insert(f.bytes.end(),p,p+sizeof(T));
}
template<class T> bool readPod(HalFile& f,T& pod) {
  if (f.position+sizeof(T)>f.bytes.size()) return false;
  memcpy(&pod,f.bytes.data()+f.position,sizeof(T));f.position+=sizeof(T);return true;
}
}
struct Store {
 std::vector<uint8_t> bytes;
 bool openFileForRead(const char*,const std::string&,HalFile& f) {f={};f.bytes=bytes;return true;}
} Storage;
struct Section {
 HalFile file; std::string filePath="sections/0.bin";uint16_t pageCount=3;
 bool collectTouchLinks_=false;int clears=0;
 void clearCache(){++clears;}
 void writeSectionFileHeader(const ReaderRenderSpec&);
 bool loadSectionFile(const ReaderRenderSpec&);
};
''' + constants + '\n' + writer + '\n' + reader + r'''
int main() {
 ReaderRenderSpec spec;
 spec.fontId=7;spec.viewportWidth=620;spec.viewportHeight=1000;
#if FREEINK_DEVICE_METALIO_PLUS || FREEINK_DEVICE_READPICO
 spec.imageOutset=24;
#endif
 spec.bookFontsKey=0x12345678;
 Section s;s.writeSectionFileHeader(spec);
 assert(s.file.bytes.size()==HEADER_SIZE);
 assert(s.file.bytes[0]==0); // Interrupted writer never publishes a valid cache.
 Storage.bytes=s.file.bytes;
 assert(!s.loadSectionFile(spec) && s.clears==1);
 Storage.bytes[0]=SECTION_FILE_VERSION;
 assert(s.loadSectionFile(spec));
 auto complete=Storage.bytes;
 // Every truncated layout header must be rejected, not read as an old-size header.
 const size_t fieldsEnd=s.file.position;
 for(size_t size=0;size<fieldsEnd;++size) {
   Storage.bytes.assign(complete.begin(),complete.begin()+size);
   assert(!s.loadSectionFile(spec));
 }
 Storage.bytes=complete;
 auto other=spec;other.viewportWidth--;
 assert(!(spec==other) && !s.loadSectionFile(other));
 Storage.bytes[0]=SECTION_FILE_PARTIAL_VERSION;
 assert(s.loadSectionFile(spec)); // Same header key for interrupted / complete builds.
 other=spec;other.bookFontsKey++;
 assert(!(spec==other) && !s.loadSectionFile(other));
 for(uint8_t old:{79,81,83,199,201,203}) {
   Storage.bytes=complete;Storage.bytes[0]=old;
   assert(!s.loadSectionFile(spec));
 }
#if FREEINK_DEVICE_METALIO_PLUS
 assert(SECTION_FILE_VERSION==125 && SECTION_FILE_PARTIAL_VERSION==157);
 other=spec;other.imageOutset=0;
 assert(!(spec==other) && !s.loadSectionFile(other));
#elif FREEINK_DEVICE_READPICO
 assert(SECTION_FILE_VERSION==123 && SECTION_FILE_PARTIAL_VERSION==159);
 other=spec;other.imageOutset=0;
 assert(!(spec==other) && !s.loadSectionFile(other));
#else
 assert(SECTION_FILE_VERSION==121 && SECTION_FILE_PARTIAL_VERSION==161);
 assert(IMAGE_LAYOUT_HEADER_SIZE==0);
#endif
}
'''

with tempfile.TemporaryDirectory(prefix='plus-image-cache-') as directory:
    work = Path(directory)
    (work / 'test.cpp').write_text(PROGRAM)
    for plus, pico in ((0, 0), (0, 1), (1, 0)):
        binary = work / f'test-{plus}-{pico}'
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', f'-DFREEINK_DEVICE_METALIO_PLUS={plus}',
                        f'-DFREEINK_DEVICE_READPICO={pico}',
                        '-I', str(EPUB), str(work / 'test.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print('Production section headers: Plus125/157, Pico123/159, generic121/161; legacy caches, font key, image-width key and truncated headers checked')
