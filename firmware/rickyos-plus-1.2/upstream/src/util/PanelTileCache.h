#pragma once

#include <GfxRenderer.h>

#include <string>
#include <utility>

// Finished artwork (covers, the home avatar) kept as panel-memory tiles, so a page that
// returns shows it again with a row copy instead of an SD read, a BMP decode and a
// per-pixel rotated draw. Tiles are byte-exact copies of what draw() produced, so a hit
// never changes the picture. Only while a 16-level frame is being painted; PSRAM only.
//
// `key` must name everything that decides those pixels, including what lies under
// artwork that leaves pixels undrawn (e.g. a selected cell's fill). `owner` is the
// source file, so a regenerated or replaced source drops its tiles.
namespace PanelTileCache {

bool restore(const GfxRenderer& renderer, const std::string& key);
void store(const GfxRenderer& renderer, const std::string& key, const std::string& owner, int x, int y, int width,
           int height);
// Whether some tile was made from `owner`: it was a valid image when drawn.
bool hasOwner(const std::string& owner);
void forget(const std::string& owner);
void clear();

template <typename Draw>
bool draw(const GfxRenderer& renderer, const std::string& key, const std::string& owner, const int x, const int y,
          const int width, const int height, Draw&& draw) {
  if (restore(renderer, key)) return true;
  if (!std::forward<Draw>(draw)()) return false;
  store(renderer, key, owner, x, y, width, height);
  return true;
}

}  // namespace PanelTileCache
