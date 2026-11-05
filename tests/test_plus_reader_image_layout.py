"""Exercise the production allocation-free chapter-art sizing policy."""
from pathlib import Path
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
APP = PORT / 'firmware/rickyos-plus-1.2'
PARSER = APP / 'upstream/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp'
SECTION = APP / 'upstream/lib/Epub/Epub/Section.cpp'

MAIN = r'''
#include <cassert>
#include <cstdint>
#include <climits>
#include "PlusReaderImageLayout.h"
using plusReaderImage::fitChapterArt;
int main() {
  int w=465,h=232;
  assert(fitChapterArt(1200,600,620,1000,0,w,h));
  assert(w==620 && h==310); // Publisher's 75% illustration now fills the column.
  // No stretching even when the old CSS width+height distorted the source.
  w=465;h=465;
  assert(fitChapterArt(1200,300,620,1000,0,w,h) && w==620 && h==155);
  w=500;h=1000;
  assert(fitChapterArt(800,1600,620,1000,0,w,h) && w==500 && h==1000);
  // Landscape orientation and a wide banner retain the same aspect.
  w=650;h=200;
  assert(fitChapterArt(1800,540,1152,580,0,w,h) && w==1152 && h==346);
  // Small bitmap, high-resolution inline glyph and hairline decoration stay small.
  w=128;h=128;
  assert(!fitChapterArt(128,128,620,1000,0,w,h) && w==128 && h==128);
  w=32;h=32;
  assert(!fitChapterArt(1024,1024,620,1000,0,w,h) && w==32 && h==32);
  w=500;h=8;
  assert(!fitChapterArt(1500,24,620,1000,0,w,h) && w==500 && h==8);
  w=100;h=300;
  assert(!fitChapterArt(1000,3000,620,1000,0,w,h) && w==100 && h==300);
  w=248;h=248;
  assert(!fitChapterArt(1000,1000,620,1000,0,w,h) && w==248 && h==248); // 40% publisher logo.
  for(int bad : {0,-1}) {
    w=465;h=200;
    assert(!fitChapterArt(1200,600,bad,1000,0,w,h));
    assert(!fitChapterArt(1200,600,620,bad,0,w,h));
  }
  // Exhaust different source / viewport aspect ratios; never overflow the page.
  for (int sw=240;sw<=8192;sw+=137) for(int sh=80;sh<=8192;sh+=293) {
    for(int vw : {320,620,1152}) for(int vh : {200,580,1000}) {
      w=vw;h=vh;
      assert(fitChapterArt(sw,sh,vw,vh,0,w,h));
      assert(w>=1 && h>=1 && w<=vw && h<=vh);
      // Aspect error is no more than half a destination pixel (rounding).
      int64_t diff=int64_t(w)*sh-int64_t(h)*sw;
      if(diff<0) diff=-diff;
      assert(diff<=(int64_t(sh)*vw<=int64_t(sw)*vh ? sw : sh)/2+1);
    }
  }
  w=INT_MAX;h=INT_MAX;
  assert(fitChapterArt(INT_MAX,INT_MAX,INT_MAX,INT_MAX,0,w,h));
  assert(w==INT_MAX && h==INT_MAX);
  using plusReaderImage::horizontalOutset;
  assert(horizontalOutset(32,32,0,0)==24);
  assert(horizontalOutset(48,32,16,0)==24);
  assert(horizontalOutset(4,4,0,0)==0);
  // Portrait 684 px, text stays at x=32, art extends to x=8..676.
  w=465;h=232;
  assert(fitChapterArt(1200,600,620,1000,24,w,h) && w==668 && h==334);
  const int relativeX=(620-w)/2;
  assert(relativeX==-24 && 32+relativeX==8 && 32+relativeX+w==676);
  w=100;h=200;
  assert(fitChapterArt(1200,600,100,1000,284,w,h) && w==668 && h==334); // Wide text margins.
  w=465;h=232;
  assert(!fitChapterArt(1200,600,620,1000,-1,w,h));
  assert(!fitChapterArt(1200,600,620,1000,INT_MAX,w,h));
  w=INT_MAX;h=INT_MAX;
  assert(!fitChapterArt(INT_MAX,INT_MAX,INT_MAX,INT_MAX,1,w,h));
  assert(!fitChapterArt(INT_MAX,INT_MAX,INT_MAX,INT_MAX,INT_MAX,w,h));
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix='plus-art-layout-') as temp:
        root = Path(temp)
        source = root / 'test.cpp'
        source.write_text('#include <initializer_list>\n' + MAIN)
        binary = root / 'test'
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I', str(APP / 'components/rickyos_application/port'),
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    parser = PARSER.read_text()
    sizing = parser.index('plusReaderImage::fitChapterArt(')
    page_break = parser.index('// Create page for image', sizing)
    image_block = parser.index('makeUniqueNoThrow<ImageBlock>', page_break)
    reservation = parser.index('self->currentPageNextY += displayHeight;', image_block)
    assert parser.index('self->pendingMarginBottom = imageMarginBottom;', reservation) > reservation
    assert sizing < page_break < image_block < reservation
    assert '#if FREEINK_DEVICE_METALIO_PLUS' in parser[:sizing][-450:]
    section = SECTION.read_text()
    assert 'SECTION_FILE_VERSION = 125;' in section and 'SECTION_FILE_VERSION = 121;' in section
    assert 'SECTION_FILE_VERSION = 123;' in section
    assert '0xFE - (SECTION_FILE_VERSION - 28)' in section
    assert 'setImageOutset(spec.imageOutset)' in section
    print('Plus chapter art: near edge-to-edge width, signed page offset, aspect, icons and page bounds passed')

if __name__ == '__main__':
    main()
