#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "RickyCos.h"

namespace ricky_ota {
#if FREEINK_DEVICE_METALIO_PLUS
// The Plus has its own manifest beside the Read Pico's (which earlier Read Pico releases
// fetch and must stay as it is); firmware files share the firmware/ folder. Diagnostic
// builds may point COS at a test folder and drop the site route.
#ifndef RICKYOS_OTA_COS_DIR
#define RICKYOS_OTA_COS_DIR "/ota/"
#endif
#ifndef RICKYOS_OTA_SITE_ROUTE
#define RICKYOS_OTA_SITE_ROUTE 1
#endif
inline constexpr char BOARD[] = "metalio-plus-s31";
inline constexpr uint16_t CHIP_ID = 32;  // ESP32-S31
inline constexpr bool SITE_ENABLED = RICKYOS_OTA_SITE_ROUTE != 0;
inline constexpr char SITE_BASE[] = "https://chinoryunqin.github.io/RickyOS-site/";
inline constexpr char MANIFEST_URL[] = "https://chinoryunqin.github.io/RickyOS-site/plus/ota.json";
inline constexpr char NOTES_URL[] = "https://chinoryunqin.github.io/RickyOS-site/plus/ota-notes.txt";
inline constexpr bool COS_ENABLED = RickyCos::ENABLED;
inline constexpr char COS_BASE[] = RICKYOS_COS_BASE RICKYOS_OTA_COS_DIR;
inline constexpr char COS_MANIFEST_URL[] = RICKYOS_COS_BASE RICKYOS_OTA_COS_DIR "plus/ota.json";
inline constexpr char COS_NOTES_URL[] = RICKYOS_COS_BASE RICKYOS_OTA_COS_DIR "plus/ota-notes.txt";
#else
inline constexpr char BOARD[] = "readpico";
inline constexpr uint16_t CHIP_ID = 9;  // ESP32-S3
inline constexpr bool SITE_ENABLED = true;
inline constexpr char SITE_BASE[] = "https://chinoryunqin.github.io/RickyOS-site/";
inline constexpr char MANIFEST_URL[] = "https://chinoryunqin.github.io/RickyOS-site/ota.json";
inline constexpr char NOTES_URL[] = "https://chinoryunqin.github.io/RickyOS-site/ota-notes.txt";
// The same files in the RickyOS COS bucket (Guangzhou), tried first: <base>ota.json,
// <base>ota-notes.txt and <base>firmware/*.bin, laid out like the site. Devices on
// earlier releases only know the site, so every release goes to both.
inline constexpr bool COS_ENABLED = RickyCos::ENABLED;
inline constexpr char COS_BASE[] = RICKYOS_COS_BASE "/ota/";
inline constexpr char COS_MANIFEST_URL[] = RICKYOS_COS_BASE "/ota/ota.json";
inline constexpr char COS_NOTES_URL[] = RICKYOS_COS_BASE "/ota/ota-notes.txt";
#endif
inline constexpr size_t SLOT_BYTES = 0x640000;
inline constexpr size_t RESERVE_BYTES = 512 * 1024;

// RickyOS versions are major.minor.patch (e.g. 1.1.0); development builds add
// "-dev" and may update to the release of the same number.
struct Version {
  std::array<uint32_t, 3> parts{};
  bool development = false;
};

inline bool parseVersion(std::string_view text, Version& result, const bool allowDevelopment = false) {
  result = {};
  const auto number = [&text](uint32_t& value) {
    if (text.empty() || text.front() < '0' || text.front() > '9') return false;
    value = 0;
    do {
      const unsigned digit = text.front() - '0';
      if (value > (UINT32_MAX - digit) / 10) return false;
      value = value * 10 + digit;
      text.remove_prefix(1);
    } while (!text.empty() && text.front() >= '0' && text.front() <= '9');
    return true;
  };
  for (size_t i = 0; i < 3; ++i) {
    if (!number(result.parts[i])) return false;
    if (i < 2) {
      if (text.empty() || text.front() != '.') return false;
      text.remove_prefix(1);
    }
  }
  if (allowDevelopment && text == "-dev") {
    result.development = true;
    return true;
  }
  return text.empty();
}

inline bool newer(std::string_view current, std::string_view candidate) {
  Version running, offered;
  if (!parseVersion(current, running, true) || !parseVersion(candidate, offered)) return false;
  return offered.parts > running.parts || (offered.parts == running.parts && running.development);
}

inline bool validFile(std::string_view file) {
  constexpr std::string_view prefix = "firmware/";
  if (file.size() >= 128 || !file.starts_with(prefix) || !file.ends_with(".bin")) return false;
  file.remove_prefix(prefix.size());
  if (file.size() <= 4) return false;
  for (const char c : file) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
          c == '-'))
      return false;
  }
  return true;
}

inline bool decodeSha(std::string_view text, std::array<uint8_t, 32>& digest) {
  if (text.size() != 64) return false;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (value < 0) return false;
    if (i % 2 == 0)
      digest[i / 2] = static_cast<uint8_t>(value << 4);
    else
      digest[i / 2] |= static_cast<uint8_t>(value);
  }
  return true;
}

// KMP state is fixed-size; no firmware buffering or per-chunk heap growth.
class StringScanner {
  std::string_view needle;
  std::array<uint8_t, 64> failure{};
  size_t matched = 0;
  bool seen = false;

 public:
  explicit StringScanner(std::string_view text) : needle(text) {
    if (text.empty() || text.size() > failure.size()) {
      needle = {};
      return;
    }
    for (size_t i = 1, j = 0; i < text.size(); ++i) {
      while (j && text[i] != text[j]) j = failure[j - 1];
      if (text[i] == text[j]) ++j;
      failure[i] = static_cast<uint8_t>(j);
    }
  }
  void feed(const uint8_t* data, size_t size) {
    if (seen || needle.empty()) return;
    for (size_t i = 0; i < size; ++i) {
      const char c = static_cast<char>(data[i]);
      while (matched && c != needle[matched]) matched = failure[matched - 1];
      if (c == needle[matched]) ++matched;
      if (matched == needle.size()) {
        seen = true;
        return;
      }
    }
  }
  bool found() const { return seen; }
};
}  // namespace ricky_ota
