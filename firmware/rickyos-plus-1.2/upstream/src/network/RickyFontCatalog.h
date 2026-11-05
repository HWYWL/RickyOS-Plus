#pragma once

#include "RickyCos.h"

// RickyOS downloads fonts from its own catalog, github.com/chinoryunqin/RickyOS-fonts,
// built by scripts/build_rickyos_font_catalog.py. Both manifests list the same files:
// one is a GitHub release asset, the other is served through jsDelivr, usually the
// faster route from mainland China. Each manifest's baseUrl stays on its own host, so
// the font files come over the route that served the list; its "mirrors" name the
// other hosts, which FontDownloadActivity tries when a file fails. When the RickyOS COS
// bucket is configured (RickyCos.h), its manifest leads in China and closes the list
// elsewhere.
namespace RickyFontCatalog {

inline constexpr char GITHUB_MANIFEST[] =
    "https://github.com/chinoryunqin/RickyOS-fonts/releases/latest/download/fonts.json";
inline constexpr char MIRROR_MANIFEST[] = "https://cdn.jsdelivr.net/gh/chinoryunqin/RickyOS-fonts@latest/mirror.json";
inline constexpr char FASTLY_MANIFEST[] =
    "https://fastly.jsdelivr.net/gh/chinoryunqin/RickyOS-fonts@latest/mirror.json";
inline constexpr const char* COS_MANIFEST = RickyCos::ENABLED ? RickyCos::FONT_MANIFEST : nullptr;
inline constexpr int MANIFEST_COUNT = 4;

// The manifest to try at each attempt, or nullptr for a route that is off (skip it).
// RickyOS readers are in mainland China, so one fixed order for every device, whatever
// its content region: the Guangzhou bucket, then jsDelivr, Fastly, and GitHub last.
inline constexpr const char* MANIFEST_ORDER[MANIFEST_COUNT] = {COS_MANIFEST, MIRROR_MANIFEST, FASTLY_MANIFEST,
                                                               GITHUB_MANIFEST};
constexpr const char* manifestFor(const int attempt) { return MANIFEST_ORDER[attempt]; }

static_assert(manifestFor(0) == COS_MANIFEST);
static_assert(manifestFor(1) == MIRROR_MANIFEST);
static_assert(manifestFor(3) == GITHUB_MANIFEST);

}  // namespace RickyFontCatalog
