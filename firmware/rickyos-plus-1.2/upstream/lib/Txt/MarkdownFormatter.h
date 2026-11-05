#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// A bounded reading-oriented Markdown subset, not a CommonMark/browser engine.
// Workspace is borrowed from the existing converter's input allocation.
class MarkdownFormatter {
 public:
  struct Sink {
    void* context;
    void (*write)(void*, std::string_view);
    void (*consume)(void*, uint32_t, const uint8_t*, uint8_t, uint8_t, bool, bool);
  };
  MarkdownFormatter(Sink sink, uint8_t* line, size_t capacity, uint8_t* pending, size_t pendingCapacity, bool gbk,
                    bool utf16 = false);
  void add(uint32_t source, const uint8_t* character, uint8_t width, uint8_t utf8Length);
  void finish();
  void beforeChapterAnchor() {
    flushPending();
    closeTable();
    closeList();
  }
  // Reuse the body inline parser for bounded, markup-free TOC labels.
  static void plainText(std::string_view text, char (&output)[192]) {
    if (text.size() >= sizeof(output)) {
      output[0] = '\0';
      return;
    }
    plainTextBounded(text, output);
  }

 private:
  static void plainTextBounded(std::string_view text, char* output);
  Sink sink_;
  uint8_t* line_;
  size_t capacity_;
  size_t length_ = 0;
  uint32_t position_ = 0;
  uint8_t* pending_;
  size_t pendingCapacity_;
  size_t pendingLength_ = 0;
  uint32_t pendingPosition_ = 0;
  uint32_t pendingNewline_ = 0;
  bool pendingHasNewline_ = false;
  static constexpr size_t MAX_TABLE_COLUMNS = 16;
  struct Cell {
    uint16_t begin, end;
  };
  char tableAlign_[MAX_TABLE_COLUMNS] = {};
  uint8_t tableColumns_ = 0;
  bool gbk_;
  bool utf16_;
  bool overflow_ = false;
  char list_ = 0;
  char fence_ = 0;
  size_t fenceLength_ = 0;

  void write(std::string_view text) { sink_.write(sink_.context, text); }
  void consume(std::string_view text, bool visible, bool spaces = false);
  void inlineText(std::string_view text, unsigned depth = 0);
  void closeList();
  void closeTable();
  void flushPending();
  static size_t splitCells(std::string_view text, Cell* cells);
  bool tableDelimiter(std::string_view text, size_t columns);
  void tableRow(std::string_view text, const Cell* cells, size_t columns, bool header);
  bool processLine();  // true: defer this line and its newline until the next line
};
