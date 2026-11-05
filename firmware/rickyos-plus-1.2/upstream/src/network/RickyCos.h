#pragma once

// The RickyOS Tencent COS bucket in Guangzhou: font and standby-picture catalogs served
// from mainland China, published by scripts/publish_rickyos_assets_cos.py. Layout:
//   <base>/<kind>/cos.json        rolling manifest (no-cache), kind = fonts | wallpapers
//   <base>/<kind>/<tag>/<file>    immutable release files
//
// RICKYOS_COS_BASE is the bucket's default HTTPS domain without a trailing slash (the
// bucket allows public read). Override with -DRICKYOS_COS_BASE=... in platformio.ini;
// -DRICKYOS_COS_BASE=\"\" turns the COS route off. Until a catalog is published there,
// the bucket answers 404 at once and the reader moves on to jsDelivr.
#ifndef RICKYOS_COS_BASE
#define RICKYOS_COS_BASE "https://rickyos-1309217881.cos.ap-guangzhou.myqcloud.com"
#endif

namespace RickyCos {

inline constexpr bool ENABLED = sizeof(RICKYOS_COS_BASE) > 1;
inline constexpr char FONT_MANIFEST[] = RICKYOS_COS_BASE "/fonts/cos.json";
inline constexpr char WALLPAPER_MANIFEST[] = RICKYOS_COS_BASE "/wallpapers/cos.json";

// A COS object is read straight from the bucket with no CDN cache in front, so its
// manifest is as current as the GitHub release: once it answers, no mirror can be newer.
constexpr bool isCosUrl(const char* url) {
  // "https://" then a host ending in ".myqcloud.com" before the path.
  if (url == nullptr) return false;
  const char* host = url;
  for (const char* p = "https://"; *p; ++p, ++host) {
    if (*host != *p) return false;
  }
  const char* end = host;
  while (*end && *end != '/') ++end;
  constexpr char kSuffix[] = ".myqcloud.com";
  constexpr int kSuffixLength = sizeof(kSuffix) - 1;
  if (end - host <= kSuffixLength) return false;
  for (int i = 0; i < kSuffixLength; ++i) {
    if (end[i - kSuffixLength] != kSuffix[i]) return false;
  }
  return true;
}

static_assert(isCosUrl("https://rickyos-1250000000.cos.ap-guangzhou.myqcloud.com/fonts/v1.0.0/"));
static_assert(!isCosUrl("https://cdn.jsdelivr.net/gh/x/y@v1/fonts/"));
static_assert(!isCosUrl("https://evil.example/x.myqcloud.com/"));
static_assert(!isCosUrl("http://a.cos.ap-guangzhou.myqcloud.com/"));
static_assert(!isCosUrl(".myqcloud.com"));

}  // namespace RickyCos
