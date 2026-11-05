"""Selected logo provenance, bounded Flash encoding and production rendering."""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_reading_ui_regressions import ROOT, run_cpp, method
from test_rickyos_navigation_icons import NODE, sharp_available

HEADER = ROOT / 'src/images/RickyLogo.h'
SHA = 'ac8c60ca236a864bb2736a2d015222aff2bc2bb667f3d7248c3d9f234863a12f'


class RickyLogoAssetTest(unittest.TestCase):
    def test_selected_image_hash_and_bounded_resources(self):
        source = ROOT / 'src/images/sources/rickyos/approved-logo.png'
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), SHA)
        text = HEADER.read_text()
        self.assertIn(SHA, text)
        arrays = re.findall(r'uint8_t (\w+)\[\] = \{([^}]+)\}', text)
        self.assertEqual(len(arrays), 2)
        data = {}
        for name, body in arrays:
            bits = bytes(int(value, 16) for value in re.findall(r'0x([0-9A-F]{2})', body))
            self.assertEqual(len(bits), 384 * 448 // 8)
            self.assertEqual(bits[:48], b'\xff' * 48)
            self.assertEqual(bits[-48:], b'\xff' * 48)
            data[name] = bits
        self.assertTrue(all((circle | full) == circle
                            for circle, full in zip(data['circleBits'], data['bits'])))

    def test_avatar_badge_square_frames_the_ring(self):
        # The default avatar maps this square onto the avatar circle, so the
        # badge's own ring becomes the edge. It must hold all badge ink and fit
        # it tightly, or the avatar shows a gap or clips the ring.
        mark = (ROOT / 'src/components/RickyBrandMark.h').read_text()
        x0, y0, size = (int(v) for v in re.search(
            r'badgeX = (\d+), badgeY = (\d+), badgeSize = (\d+)', mark).groups())
        text = HEADER.read_text()
        top = int(re.search(r'wordmarkTop = (\d+)', text).group(1))
        body = re.search(r'uint8_t bits\[\] = \{([^}]+)\}', text).group(1)
        bits = bytes(int(value, 16) for value in re.findall(r'0x([0-9A-F]{2})', body))
        ink = [(x, y) for y in range(top) for x in range(384) if not bits[y * 48 + x // 8] & (0x80 >> (x % 8))]
        xs, ys = [x for x, _ in ink], [y for _, y in ink]
        self.assertTrue(x0 <= min(xs) and max(xs) < x0 + size and y0 <= min(ys) and max(ys) < y0 + size)
        self.assertLessEqual(min(xs) - x0 + x0 + size - 1 - max(xs), 4)
        self.assertLessEqual(abs(size - (max(ys) - min(ys) + 1)), 16)

    @unittest.skipUnless(sharp_available(), 'regeneration requires developer Node.js + sharp')
    def test_regeneration_matches_approved_resource(self):
        with tempfile.TemporaryDirectory(prefix='ricky-logo-build-') as directory:
            output = Path(directory) / 'logo.h'
            command = [NODE, str(ROOT / 'scripts/build_rickyos_logo.cjs'), '--out', str(output)]
            subprocess.run(command, check=True)
            self.assertEqual(output.read_bytes(), HEADER.read_bytes())
            first = output.read_bytes()
            subprocess.run(command, check=True)
            self.assertEqual(output.read_bytes(), first)

    def test_actual_logo_renderer_no_clipping_or_extra_pixels(self):
        run_cpp(r'''
#include <cassert>
#include "components/RickyBrandMark.h"
struct Renderer {
  mutable bool pixels[480][480]{};
  void drawPixel(int x,int y,bool black) const {
    assert(x>=0 && x<480 && y>=0 && y<480 && black);pixels[y][x]=true;
  }
};
int main() {
  for(auto box : {Rect{10,10,384,448},Rect{20,20,180,180},Rect{20,20,240,100},
                 Rect{5,5,0,0},Rect{5,5,0,30}}) for(uint8_t phase : {0,1,2,3}) {
    Renderer renderer;RickyBrandMark::draw(renderer,box,phase);
    const auto m=RickyBrandMark::fit(box);
    for(int y=0;y<480;++y) for(int x=0;x<480;++x) {
      bool ink=false;
      if(m.width && m.height && x>=m.x && x<m.x+m.width && y>=m.y && y<m.y+m.height)
        ink=RickyBrandMark::pixel((x-m.x)*RickyLogoAsset::width/m.width,
                                  (y-m.y)*RickyLogoAsset::height/m.height,phase);
      assert(renderer.pixels[y][x]==ink);
    }
  }
  assert(!RickyBrandMark::pixel(-1,0,3)); assert(!RickyBrandMark::pixel(384,0,3));
  assert(!RickyBrandMark::pixel(0,448,3));
}
''', include_dirs=(ROOT / 'src',))

    def test_power_scenes_use_embedded_artwork_and_no_duplicate_wordmark(self):
        source = (ROOT / 'src/components/themes/BaseTheme.cpp').read_text()
        draw = method(source, 'void BaseTheme::drawRickyPowerScreen(')
        self.assertIn('RickyBrandMark::draw(renderer, layout.mark, phase)', draw)
        self.assertIn('constexpr uint8_t phase = 3', draw)
        self.assertNotIn('RickyBrandMark::strokes', draw)
        self.assertNotIn('tr(STR_CROSSPOINT)', draw)
        for allocation in ('new ', 'malloc(', 'std::vector', 'Storage.'):
            self.assertNotIn(allocation, draw)
        boot = (ROOT / 'src/activities/boot_sleep/BootActivity.cpp').read_text()
        splash = method(boot, 'void BootActivity::renderSplash(').split('#endif', 1)[0]
        self.assertIn('drawRickyPowerScreen(renderer, false, false)', splash)
        self.assertEqual(splash.count('displayBuffer('), 1)
        self.assertNotIn('CROSSPOINT_VERSION', splash)
        self.assertNotIn('delay(', splash)
        self.assertNotIn('for (', splash)


if __name__ == '__main__':
    unittest.main()
