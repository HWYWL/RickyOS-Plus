#pragma once

#include "RickyCos.h"

// Separate from the ordinary font catalog: automatic reader fallback requires
// the original NotoSansSC coverage and the exact cpfont ABI, not a UI subset.
namespace RickyReaderResources {

inline constexpr bool ENABLED = RickyCos::ENABLED;
inline constexpr char FONT_MANIFEST_FORMAT[] = RICKYOS_COS_BASE "/font-fallback/m%s-b%s/cos.json";
inline constexpr char DICTIONARY_MANIFEST_FORMAT[] = RICKYOS_COS_BASE "/dictionaries/m%u/cos.json";

// Match the owned origin and its slash boundary, not a suffix host or user-info.
// This is a catalog routing guard, not a replacement for downloader/TLS checks.
constexpr bool isOwnedUrl(const char* url) {
  if (!ENABLED || !url) return false;
  const char* base = RICKYOS_COS_BASE;
  for (; *base; ++base, ++url) {
    if (*url != *base) return false;
  }
  return *url == '/';
}

}  // namespace RickyReaderResources
