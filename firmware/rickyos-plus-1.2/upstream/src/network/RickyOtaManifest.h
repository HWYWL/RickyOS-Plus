#pragma once

#include "RickyOtaPolicy.h"

// A deliberately small, flat JSON contract generated from the web catalog.
// Strict punctuation, types, duplicates, completion and length checks. The
// publisher emits plain UTF-8 strings; unsupported escapes fail closed.
class RickyOtaManifest {
  enum class State : uint8_t { Start, KeyOrEnd, Key, Colon, Value, String, Primitive, CommaOrEnd, Done, Failed };
  enum Field : uint8_t {
    Schema,
    Product,
    Board,
    Channel,
    Status,
    Version,
    File,
    Bytes,
    Sha,
    Approved,
    Accepted,
    Chip,
    Flash,
    Count
  };
  State state = State::Start;
  Field field = Schema;
  uint16_t seen = 0;
  size_t received = 0;
  size_t tokenLength = 0;
  char token[128]{};
  bool escaped = false;
  bool allowEnd = true;
  bool available = false;
  char version[48]{};
  char file[128]{};
  std::array<uint8_t, 32> digest{};
  size_t imageBytes = 0;

  static constexpr bool space(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
  static constexpr bool numeric(Field f) { return f == Schema || f == Bytes || f == Chip || f == Flash; }
  static constexpr bool boolean(Field f) { return f == Approved || f == Accepted; }
  bool append(char c) {
    if (tokenLength >= sizeof(token) - 1) return false;
    token[tokenLength++] = c;
    token[tokenLength] = '\0';
    return true;
  }
  bool commitKey() {
    static constexpr const char* keys[] = {"schema",           "product", "board",     "channel", "status",
                                           "version",          "file",    "bytes",     "sha256",  "approved",
                                           "hardwareAccepted", "chipId",  "flashBytes"};
    for (uint8_t i = 0; i < Count; ++i) {
      if (std::string_view(token, tokenLength) != keys[i]) continue;
      field = static_cast<Field>(i);
      if (seen & (1u << i)) return false;
      seen |= 1u << i;
      return true;
    }
    return false;
  }
  bool commitString() {
    const std::string_view text(token, tokenLength);
    switch (field) {
      case Product:
        return text == "RickyOS";
      case Board:
        return text == ricky_ota::BOARD;
      case Channel:
        return text == "stable";
      case Status:
        available = text == "update_available";
        return available || text == "no_update";
      case Version: {
        ricky_ota::Version parsed;
        if (text.size() >= sizeof(version) || !ricky_ota::parseVersion(text, parsed)) return false;
        std::memcpy(version, token, tokenLength + 1);
        return true;
      }
      case File:
        if (!ricky_ota::validFile(text)) return false;
        std::memcpy(file, token, tokenLength + 1);
        return true;
      case Sha:
        return ricky_ota::decodeSha(text, digest);
      default:
        return false;
    }
  }
  bool commitPrimitive() {
    const std::string_view text(token, tokenLength);
    if (boolean(field)) return text == "true";
    if (!numeric(field) || text.empty() || (text.size() > 1 && text.front() == '0')) return false;
    uint32_t value = 0;
    for (const char c : text) {
      if (c < '0' || c > '9' || value > (UINT32_MAX - static_cast<unsigned>(c - '0')) / 10) return false;
      value = value * 10 + c - '0';
    }
    if (field == Schema) return value == 1;
    if (field == Chip) return value == ricky_ota::CHIP_ID;
    if (field == Flash) return value == 0x1000000;
    imageBytes = value;
    return value >= 24 && value % 4 == 0 && value <= ricky_ota::SLOT_BYTES - ricky_ota::RESERVE_BYTES &&
           ((value + 4095u) & ~4095u) <= ricky_ota::SLOT_BYTES - ricky_ota::RESERVE_BYTES;
  }
  bool consume(char c) {
    switch (state) {
      case State::Start:
        if (space(c)) return true;
        if (c != '{') return false;
        state = State::KeyOrEnd;
        return true;
      case State::KeyOrEnd:
        if (space(c)) return true;
        if (c == '}' && allowEnd) {
          state = State::Done;
          return true;
        }
        if (c != '"') return false;
        tokenLength = 0;
        escaped = false;
        state = State::Key;
        return true;
      case State::Key:
      case State::String:
        if (escaped) {
          escaped = false;
          return (c == '"' || c == '\\' || c == '/') && append(c);
        }
        if (c == '\\') {
          escaped = true;
          return true;
        }
        if (static_cast<unsigned char>(c) < 0x20) return false;
        if (c != '"') return append(c);
        if (state == State::Key) {
          if (!commitKey()) return false;
          state = State::Colon;
        } else {
          if (!commitString()) return false;
          state = State::CommaOrEnd;
        }
        return true;
      case State::Colon:
        if (space(c)) return true;
        if (c != ':') return false;
        state = State::Value;
        return true;
      case State::Value:
        if (space(c)) return true;
        tokenLength = 0;
        escaped = false;
        if (c == '"' && !numeric(field) && !boolean(field)) {
          state = State::String;
          return true;
        }
        if (!numeric(field) && !boolean(field)) return false;
        state = State::Primitive;
        return append(c);
      case State::Primitive:
        if (!space(c) && c != ',' && c != '}') return append(c);
        if (!commitPrimitive()) return false;
        state = State::CommaOrEnd;
        return consume(c);
      case State::CommaOrEnd:
        if (space(c)) return true;
        if (c == '}') {
          state = State::Done;
          return true;
        }
        if (c != ',') return false;
        allowEnd = false;
        state = State::KeyOrEnd;
        return true;
      case State::Done:
        return space(c);
      case State::Failed:
        return false;
    }
    return false;
  }

 public:
  bool feed(const uint8_t* data, size_t size) {
    if (size > 2048 - received) {
      state = State::Failed;
      return false;
    }
    received += size;
    for (size_t i = 0; i < size; ++i) {
      if (!consume(static_cast<char>(data[i]))) {
        state = State::Failed;
        return false;
      }
    }
    return true;
  }
  bool valid() const {
    constexpr uint16_t base = (1u << Schema) | (1u << Product) | (1u << Board) | (1u << Channel) | (1u << Status);
    return state == State::Done && seen == (available ? (1u << Count) - 1 : base);
  }
  bool hasUpdate() const { return valid() && available; }
  const char* getVersion() const { return version; }
  const char* getFile() const { return file; }
  const std::array<uint8_t, 32>& getSha() const { return digest; }
  size_t getBytes() const { return imageBytes; }
};
