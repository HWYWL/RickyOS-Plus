#include "PanelTileCache.h"

#include <Memory.h>

#include <array>
#include <cstdlib>
#include <mutex>

namespace PanelTileCache {
namespace {
// A library page holds six covers and the home screen four plus the avatar; this keeps
// a few pages of each. One 4-bit cover tile is ~20 KB.
constexpr size_t kMaxTiles = 40;
constexpr size_t kMaxBytes = 1024 * 1024;
constexpr size_t kMaxTileBytes = 128 * 1024;

struct Tile {
  std::string key;
  std::string owner;
  GfxRenderer::PanelTile geometry;
  memory::ByteBuffer bytes;
  size_t size = 0;
  uint32_t lastUse = 0;
};

std::array<Tile, kMaxTiles> tiles;
size_t usedBytes = 0;
uint32_t clock = 0;
std::mutex lock;

memory::ByteBuffer allocate(const size_t size) {
#if defined(SIMULATOR)
  return memory::ByteBuffer{static_cast<uint8_t*>(std::malloc(size))};
#else
  // Never compete with readers and decoders for the last of PSRAM.
  if (!memory::psramHasHeadroom(size, size, 64 * 1024)) return {};
  return memory::makePsramByteBufferUninitializedNoThrow(size);
#endif
}

void drop(Tile& tile) {
  usedBytes -= tile.size;
  tile = Tile{};
}

Tile* find(const std::string& key) {
  for (auto& tile : tiles)
    if (tile.bytes && tile.key == key) return &tile;
  return nullptr;
}
}  // namespace

bool restore(const GfxRenderer& renderer, const std::string& key) {
  if (!renderer.isGrayscale16Active()) return false;
  const std::lock_guard<std::mutex> guard(lock);
  Tile* tile = find(key);
  if (!tile || !renderer.restorePanelTile(tile->geometry, tile->bytes.get())) return false;
  tile->lastUse = ++clock;
  return true;
}

void store(const GfxRenderer& renderer, const std::string& key, const std::string& owner, const int x, const int y,
           const int width, const int height) {
  if (!renderer.isGrayscale16Active()) return;
  GfxRenderer::PanelTile geometry;
  if (!renderer.panelTileFor(x, y, width, height, &geometry)) return;
  const size_t size = geometry.bytes();
  if (size > kMaxTileBytes) return;
  const std::lock_guard<std::mutex> guard(lock);
  if (Tile* old = find(key)) drop(*old);
  // Least recently used first, until the new tile fits in both limits.
  Tile* slot = nullptr;
  while (true) {
    Tile* oldest = nullptr;
    for (auto& tile : tiles) {
      if (!tile.bytes) {
        if (!slot) slot = &tile;
        continue;
      }
      if (!oldest || tile.lastUse < oldest->lastUse) oldest = &tile;
    }
    if (slot && usedBytes + size <= kMaxBytes) break;
    if (!oldest) return;
    drop(*oldest);
    if (!slot) slot = oldest;
  }
  auto bytes = allocate(size);
  if (!bytes) return;
  renderer.capturePanelTile(geometry, bytes.get());
  slot->key = key;
  slot->owner = owner;
  slot->geometry = geometry;
  slot->bytes = std::move(bytes);
  slot->size = size;
  slot->lastUse = ++clock;
  usedBytes += size;
}

bool hasOwner(const std::string& owner) {
  const std::lock_guard<std::mutex> guard(lock);
  for (const auto& tile : tiles)
    if (tile.bytes && tile.owner == owner) return true;
  return false;
}

void forget(const std::string& owner) {
  const std::lock_guard<std::mutex> guard(lock);
  for (auto& tile : tiles)
    if (tile.bytes && tile.owner == owner) drop(tile);
}

void clear() {
  const std::lock_guard<std::mutex> guard(lock);
  for (auto& tile : tiles)
    if (tile.bytes) drop(tile);
}

}  // namespace PanelTileCache
