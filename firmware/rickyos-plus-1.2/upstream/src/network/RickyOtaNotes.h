#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

// ota-notes.txt from the RickyOS site (tools/rickyos-flasher/scripts/build-ota.js):
// the offered version on the first line, then one note per line. A separate file,
// because devices from 1.1.2 refuse any key ota.json does not already carry.
// Fails closed: a different version, an overlong line or a control byte shows no
// notes, and the update itself never depends on them.
template <size_t N>
class RickyOtaNotes {
  static constexpr size_t kMaxBytes = 2048;
  std::span<std::array<char, N>> notes_;
  std::string_view version_;
  char line_[N]{};
  size_t lineLength_ = 0;
  size_t count_ = 0;
  size_t received_ = 0;
  bool versionSeen_ = false;
  bool failed_ = false;

  bool commitLine() {
    const std::string_view text(line_, lineLength_);
    lineLength_ = 0;
    if (!versionSeen_) {
      versionSeen_ = true;
      return text == version_;
    }
    if (text.empty()) return true;
    if (count_ >= notes_.size()) return true;  // more than shown: keep the first ones
    std::memcpy(notes_[count_].data(), text.data(), text.size());
    notes_[count_][text.size()] = '\0';
    ++count_;
    return true;
  }

 public:
  RickyOtaNotes(std::span<std::array<char, N>> notes, std::string_view version) : notes_(notes), version_(version) {}

  bool feed(const uint8_t* data, size_t size) {
    if (failed_ || size > kMaxBytes - received_) return !(failed_ = true);
    received_ += size;
    for (size_t i = 0; i < size; ++i) {
      const char c = static_cast<char>(data[i]);
      if (c == '\r') continue;
      if (c == '\n') {
        if (!commitLine()) return !(failed_ = true);
        continue;
      }
      if (static_cast<unsigned char>(c) < 0x20 || lineLength_ >= N - 1) return !(failed_ = true);
      line_[lineLength_++] = c;
    }
    return true;
  }

  // Notes ready to show; 0 unless the whole file ended cleanly for this version.
  size_t count() const { return !failed_ && versionSeen_ && lineLength_ == 0 ? count_ : 0; }
};
