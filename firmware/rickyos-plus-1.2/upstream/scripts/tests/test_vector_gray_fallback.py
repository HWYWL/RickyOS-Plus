"""Compile production glyph decoding and reader native-gray failure branches."""
from pathlib import Path
import unittest
from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class VectorGrayFallbackTest(unittest.TestCase):
    def test_four_bit_packing_preserves_all_legacy_two_bit_thresholds(self):
        ttf = (ROOT / 'lib/EpdFont/TtfEpdFont.cpp').read_text()
        renderer = (ROOT / 'lib/GfxRenderer/GfxRenderer.cpp').read_text()
        packing = method(ttf, 'if (f.fourBit)')
        helpers = ''.join(method(renderer, name) for name in (
            'static uint8_t get4BitCoverage(', 'static uint8_t get2BitCoverage('))
        run_cpp(r'''
#include <cassert>
#include <cstdint>
''' + helpers + r'''
int main(){
 uint8_t dst[128]{};
 struct {bool fourBit=true;} f;
 for(int i=0;i<256;++i){
  const uint8_t a=static_cast<uint8_t>(i);
''' + packing + r'''
  const uint8_t legacy=a<64?0:a<128?1:a<192?2:3;
  assert(get2BitCoverage(dst,i,true)==legacy);
 }
}
''')


if __name__ == '__main__':
    unittest.main()
