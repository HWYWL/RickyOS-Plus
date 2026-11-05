#pragma once

#include <cstddef>
#include <cstdint>

// Offline decoder, independent of HAL, fonts and UI. No heap, SD or network
// access per key. One activity owns one session; no global mutable state.
class PinyinInput {
 public:
  static constexpr size_t MAX_RAW = 48;
  static constexpr size_t PAGE_SIZE = 5;
  static constexpr size_t MAX_TEXT = 73;  // 24 basic Han characters + NUL
  struct Candidate {
    char text[MAX_TEXT]{};
    uint8_t consume = 0;  // raw byte count, including explicit apostrophes
  };
  bool append(char letter);
  bool backspace();
  void clear();
  const char* raw() const { return composition; }
  bool empty() const { return length == 0; }
  size_t count() const { return candidateCount; }
  const Candidate* candidate(size_t index) const { return index < candidateCount ? &page[index] : nullptr; }
  bool nextPage();
  bool previousPage();
  bool consume(size_t index);  // only call AFTER successful insertion
  bool hasNext() const { return more; }
  bool hasPrevious() const { return skip != 0; }
  void resetSession();  // clear composition AND transient preference history

 private:
  char composition[MAX_RAW + 1]{};
  Candidate page[PAGE_SIZE]{};
  uint8_t length = 0;
  uint8_t candidateCount = 0;
  uint16_t skip = 0;
  bool more = false;
  // Bounded DP and session-only learning. No input is sent to a server and
  // nothing is written to SD on each keystroke.
  int16_t scores[MAX_RAW + 1]{};
  uint8_t previous[MAX_RAW + 1]{};
  uint16_t tokens[MAX_RAW + 1]{};
  struct Learned {
    char key[13]{};
    char text[13]{};
  };
  Learned learned[16]{};
  uint8_t learnedNext = 0;
  char best[MAX_TEXT]{};
  void refresh();
  void compose();
};
