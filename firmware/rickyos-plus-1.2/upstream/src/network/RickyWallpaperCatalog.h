#pragma once

#include <algorithm>
#include <cstdint>

#include "RickyCos.h"

// RickyOS default standby pictures, github.com/chinoryunqin/RickyOS-wallpapers, built by
// scripts/build_rickyos_wallpapers.py. Same routes as RickyFontCatalog: a GitHub release
// manifest, a jsDelivr one (two CDN hosts) and, when configured, the RickyOS COS bucket
// (RickyCos.h); each lists the other hosts under "mirrors".
namespace RickyWallpaperCatalog {

inline constexpr char GITHUB_MANIFEST[] =
    "https://github.com/chinoryunqin/RickyOS-wallpapers/releases/latest/download/wallpapers.json";
inline constexpr char MIRROR_MANIFEST[] =
    "https://cdn.jsdelivr.net/gh/chinoryunqin/RickyOS-wallpapers@latest/mirror.json";
inline constexpr char FASTLY_MANIFEST[] =
    "https://fastly.jsdelivr.net/gh/chinoryunqin/RickyOS-wallpapers@latest/mirror.json";
inline constexpr const char* COS_MANIFEST = RickyCos::ENABLED ? RickyCos::WALLPAPER_MANIFEST : nullptr;
inline constexpr int MANIFEST_COUNT = 4;
// Card folder the pictures land in; the standby picker starts in /images.
inline constexpr char FOLDER[] = "/images/待机图片";
// Transparent stickers (the manifest's "stickers" list, PNG with alpha laid over the
// kept page). Older firmware reads only "items", so it never mistakes one for a picture.
inline constexpr char STICKER_FOLDER[] = "/images/透明壁纸";

// The manifest to try at each attempt, or nullptr for a route that is off (skip it).
// One fixed order for every device (RickyOS readers are in mainland China): the
// Guangzhou bucket, then jsDelivr, Fastly, and GitHub last.
inline constexpr const char* MANIFEST_ORDER[MANIFEST_COUNT] = {COS_MANIFEST, MIRROR_MANIFEST, FASTLY_MANIFEST,
                                                               GITHUB_MANIFEST};
constexpr const char* manifestFor(const int attempt) { return MANIFEST_ORDER[attempt]; }
static_assert(manifestFor(0) == COS_MANIFEST);
static_assert(manifestFor(3) == GITHUB_MANIFEST);

// The release in a manifest's baseUrl ("...@v1.1.0/..." or ".../download/v1.1.0/") as
// 0xMMmmpp, 0 when absent, so manifests from routes with stale caches can be ranked.
constexpr uint32_t releaseOf(const char* url) {
  for (const char* p = url; p && *p; ++p) {
    if ((*p != '@' && *p != '/') || p[1] != 'v' || p[2] < '0' || p[2] > '9') continue;
    uint32_t parts[3] = {0, 0, 0};
    int part = 0;
    for (const char* q = p + 2; *q && part < 3; ++q) {
      if (*q >= '0' && *q <= '9') {
        parts[part] = parts[part] * 10 + static_cast<uint32_t>(*q - '0');
      } else if (*q == '.') {
        ++part;
      } else {
        break;
      }
    }
    return (std::min<uint32_t>(parts[0], 255) << 16) | (std::min<uint32_t>(parts[1], 255) << 8) |
           std::min<uint32_t>(parts[2], 255);
  }
  return 0;
}
static_assert(releaseOf("https://cdn.jsdelivr.net/gh/x/y@v1.1.0/wallpapers/") == 0x010100);
static_assert(releaseOf("https://github.com/x/y/releases/download/v1.0.0/") == 0x010000);
static_assert(releaseOf("https://example.com/plain/") == 0);
static_assert(releaseOf("https://rickyos-1250000000.cos.ap-guangzhou.myqcloud.com/wallpapers/v1.3.0/") == 0x010300);

}  // namespace RickyWallpaperCatalog
