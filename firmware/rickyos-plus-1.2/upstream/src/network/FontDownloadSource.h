#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>

enum class FontDownloadSource { Other, Cos, JsDelivr, Fastly, GitHub };

// Host only: never retain URL paths, signed queries or userinfo. Fixed activity storage.
inline void copyFontDownloadHost(char* output, const size_t capacity, const char* host) {
  if (capacity == 0) return;
  output[0] = '\0';
  if (!host) return;
  if (const char* at = std::strrchr(host, '@')) host = at + 1;
  size_t length = 0;
  while (*host && length + 1 < capacity) {
    char c = *host++;
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
          c == ':' || c == '[' || c == ']')) break;
    output[length++] = c;
  }
  output[length] = '\0';
}

inline FontDownloadSource classifyFontDownloadHost(const char* host) {
  const std::string_view name(host);
  if (name.ends_with(".cos.ap-guangzhou.myqcloud.com")) return FontDownloadSource::Cos;
  if (name == "cdn.jsdelivr.net") return FontDownloadSource::JsDelivr;
  if (name == "fastly.jsdelivr.net") return FontDownloadSource::Fastly;
  if (name == "github.com" || name.ends_with(".githubusercontent.com")) return FontDownloadSource::GitHub;
  return FontDownloadSource::Other;
}
