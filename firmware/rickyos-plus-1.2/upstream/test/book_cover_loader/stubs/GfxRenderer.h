#pragma once

#include <cstddef>
#include <cstdint>

// The loader only forgets cover tiles; no frame is ever painted in these tests.
class GfxRenderer {
 public:
  struct PanelTile {
    int16_t lx0 = 0, ly0 = 0, lx1 = 0, ly1 = 0;
    int16_t px0 = 0, py0 = 0, px1 = 0, py1 = 0;
    uint8_t orientation = 0;
    size_t bytes() const { return 0; }
  };
  bool isGrayscale16Active() const { return false; }
  bool panelTileFor(int, int, int, int, PanelTile*) const { return false; }
  void capturePanelTile(const PanelTile&, uint8_t*) const {}
  bool restorePanelTile(const PanelTile&, const uint8_t*) const { return false; }
};
