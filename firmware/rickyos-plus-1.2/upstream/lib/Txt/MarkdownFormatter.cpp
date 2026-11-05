#include "MarkdownFormatter.h"

#include <cstring>

#include "TxtChapterIndex.h"

MarkdownFormatter::MarkdownFormatter(Sink sink, uint8_t* line, size_t capacity, uint8_t* pending,
                                     size_t pendingCapacity, bool gbk, bool utf16)
    : sink_(sink),
      line_(line),
      capacity_(capacity),
      pending_(pending),
      pendingCapacity_(pendingCapacity),
      gbk_(gbk),
      utf16_(utf16) {}

void MarkdownFormatter::plainTextBounded(std::string_view text, char* output) {
  // Inline markup only removes source bytes, never expands the label. Validate
  // once instead of checking remaining capacity for every UTF-8 character.
  char* cursor = output;
  MarkdownFormatter formatter(
      {&cursor, [](void*, std::string_view) {},
       [](void* opaque, uint32_t, const uint8_t* bytes, uint8_t, uint8_t length, bool visible, bool) {
         auto& cursor = *static_cast<char**>(opaque);
         if (visible) {
           // In-place labels always write at or behind the consumed source.
           // A forward byte copy handles overlap without a general memmove.
           for (uint8_t i = 0; i < length; ++i) *cursor++ = static_cast<char>(bytes[i]);
         }
       }},
      nullptr, 0, nullptr, 0, false);
  formatter.inlineText(text);
  *cursor = '\0';
}

void MarkdownFormatter::consume(std::string_view text, const bool visible, const bool spaces) {
  for (size_t i = 0; i < text.size();) {
    const uint8_t first = static_cast<uint8_t>(text[i]);
    uint8_t utf8Length = first < 0x80 ? 1 : first < 0xE0 ? 2 : first < 0xF0 ? 3 : 4;
    if (utf8Length > text.size() - i) utf8Length = 1;
    const uint8_t width = utf16_ ? (utf8Length == 4 ? 4 : 2) : gbk_ && first >= 0x80 ? 2 : utf8Length;
    sink_.consume(sink_.context, position_, reinterpret_cast<const uint8_t*>(text.data() + i), width, utf8Length,
                  visible && first != '\r', spaces);
    position_ += width;
    i += utf8Length;
  }
}

void MarkdownFormatter::inlineText(std::string_view text, const unsigned depth) {
  // Bound recursion and nesting. Unsupported/unmatched syntax remains literal.
  if (depth >= 4) {
    consume(text, true);
    return;
  }
  bool previousWord = false;
  while (!text.empty()) {
    if (text.front() == '\\' && text.size() > 1 && strchr("\\`*_{}[]()#+-.!>|", text[1])) {
      consume(text.substr(0, 1), false);
      consume(text.substr(1, 1), true);
      text.remove_prefix(2);
      previousWord = false;
      continue;
    }
    const char marker = text.front();
    const bool doubleMarker = text.size() > 1 && text[1] == marker && (marker == '*' || marker == '_');
    const size_t width = doubleMarker ? 2 : 1;
    if (marker == '*' || (marker == '_' && !previousWord) || marker == '`') {
      const auto delimiter = text.substr(0, width);
      const auto end = text.find(delimiter, width);
      if (end != std::string_view::npos && end > width && (marker == '`' || text[width] != ' ')) {
        const char* open = marker == '`' ? "<code>" : doubleMarker ? "<strong>" : "<em>";
        const char* close = marker == '`' ? "</code>" : doubleMarker ? "</strong>" : "</em>";
        consume(delimiter, false);
        write(open);
        if (marker == '`')
          consume(text.substr(width, end - width), true, true);
        else
          inlineText(text.substr(width, end - width), depth + 1);
        write(close);
        consume(text.substr(end, width), false);
        text.remove_prefix(end + width);
        previousWord = true;
        continue;
      }
    }
    // Display link text, never fetch destinations or inject raw HTML/attributes.
    if (marker == '[' || (marker == '!' && text.size() > 1 && text[1] == '[')) {
      const size_t labelStart = marker == '!' ? 2 : 1;
      const auto labelEnd = text.find("](");
      const auto urlEnd = labelEnd == std::string_view::npos ? labelEnd : text.find(')', labelEnd + 2);
      if (labelEnd != std::string_view::npos && labelEnd >= labelStart && urlEnd != std::string_view::npos) {
        consume(text.substr(0, labelStart), false);
        inlineText(text.substr(labelStart, labelEnd - labelStart), depth + 1);
        consume(text.substr(labelEnd, urlEnd + 1 - labelEnd), false);
        text.remove_prefix(urlEnd + 1);
        previousWord = true;
        continue;
      }
    }
    // Flush a literal run, retaining UTF-8 boundaries and intraword underscores.
    size_t count = 1;
    while (count < text.size() && !strchr("\\*_`[", text[count])) ++count;
    while (count < text.size() && (static_cast<uint8_t>(text[count]) & 0xC0) == 0x80) ++count;
    consume(text.substr(0, count), true);
    previousWord = text[count - 1] != ' ' && text[count - 1] != '\t';
    text.remove_prefix(count);
  }
}

void MarkdownFormatter::closeList() {
  if (list_) write(list_ == 'o' ? "</ol>" : "</ul>");
  list_ = 0;
}

void MarkdownFormatter::closeTable() {
  if (tableColumns_) write("</table>");
  tableColumns_ = 0;
}

void MarkdownFormatter::flushPending() {
  if (!pendingLength_) return;
  const uint32_t current = position_;
  position_ = pendingPosition_;
  write("<p>");
  inlineText(std::string_view(reinterpret_cast<const char*>(pending_), pendingLength_));
  write("</p>");
  if (pendingHasNewline_) {
    const uint8_t newline = '\n';
    sink_.consume(sink_.context, pendingNewline_, &newline, utf16_ ? 2 : 1, 1, false, false);
  }
  pendingLength_ = 0;
  position_ = current;
}

size_t MarkdownFormatter::splitCells(std::string_view text, Cell* cells) {
  size_t begin = 0, end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
  bool pipe = false;
  if (begin < end && text[begin] == '|') {
    ++begin;
    pipe = true;
  }
  size_t count = 0, start = begin;
  for (size_t i = begin; i < end; ++i) {
    if (text[i] == '\\' && i + 1 < end) {
      ++i;
      continue;
    }
    if (text[i] == '`') {
      size_t run = 1;
      while (i + run < end && text[i + run] == '`') ++run;
      const size_t close = text.find(text.substr(i, run), i + run);
      if (close != std::string_view::npos && close < end) {
        i = close + run - 1;
        continue;
      }
    }
    if (text[i] != '|') continue;
    pipe = true;
    if (count == MAX_TABLE_COLUMNS) return 0;  // literal fallback, never drop extra cells
    cells[count++] = {static_cast<uint16_t>(start), static_cast<uint16_t>(i)};
    start = i + 1;
  }
  if (!pipe) return 0;
  if (start < end || !count) {
    if (count == MAX_TABLE_COLUMNS) return 0;
    cells[count++] = {static_cast<uint16_t>(start), static_cast<uint16_t>(end)};
  }
  return count;
}

bool MarkdownFormatter::tableDelimiter(std::string_view text, size_t columns) {
  Cell cells[MAX_TABLE_COLUMNS];
  if (splitCells(text, cells) != columns) return false;
  for (size_t i = 0; i < columns; ++i) {
    auto cell = text.substr(cells[i].begin, cells[i].end - cells[i].begin);
    while (!cell.empty() && (cell.front() == ' ' || cell.front() == '\t')) cell.remove_prefix(1);
    while (!cell.empty() && (cell.back() == ' ' || cell.back() == '\t')) cell.remove_suffix(1);
    const bool left = !cell.empty() && cell.front() == ':';
    if (left) cell.remove_prefix(1);
    const bool right = !cell.empty() && cell.back() == ':';
    if (right) cell.remove_suffix(1);
    if (cell.empty()) return false;
    for (char value : cell)
      if (value != '-') return false;
    tableAlign_[i] = left && right ? 'c' : right ? 'r' : 'l';
  }
  return true;
}

void MarkdownFormatter::tableRow(std::string_view text, const Cell* cells, size_t columns, bool header) {
  write("<tr>");
  size_t consumed = 0;
  for (size_t i = 0; i < tableColumns_; ++i) {
    write(header ? "<th style=\"text-align:" : "<td style=\"text-align:");
    write(tableAlign_[i] == 'c' ? "center\">" : tableAlign_[i] == 'r' ? "right\">" : "left\">");
    if (i < columns) {
      size_t begin = cells[i].begin, end = cells[i].end;
      while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
      while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
      consume(text.substr(consumed, begin - consumed), false);
      inlineText(text.substr(begin, end - begin));
      consumed = end;
    }
    write(header ? "</th>" : "</td>");
  }
  consume(text.substr(consumed), false);
  write("</tr>");
}

bool MarkdownFormatter::processLine() {
  std::string_view text(reinterpret_cast<const char*>(line_), length_);
  if (pendingLength_) {
    Cell cells[MAX_TABLE_COLUMNS];
    const std::string_view pending(reinterpret_cast<const char*>(pending_), pendingLength_);
    const size_t columns = splitCells(pending, cells);
    if (!overflow_ && tableDelimiter(text, columns)) {
      const uint32_t current = position_;
      position_ = pendingPosition_;
      tableColumns_ = static_cast<uint8_t>(columns);
      write("<table>");
      tableRow(pending, cells, columns, true);
      if (pendingHasNewline_) {
        const uint8_t newline = '\n';
        sink_.consume(sink_.context, pendingNewline_, &newline, utf16_ ? 2 : 1, 1, false, false);
      }
      pendingLength_ = 0;
      position_ = current;
      consume(text, false);
      return false;
    }
    flushPending();
  }
  if (overflow_) {
    consume(text, true, fence_ != 0);
    write(fence_ ? "<br />" : "</p>");
    overflow_ = false;
    return false;
  }
  size_t indent = 0;
  while (indent < text.size() && indent < 3 && text[indent] == ' ') ++indent;
  auto body = text.substr(indent);
  size_t trimmed = body.size();
  while (trimmed && (body[trimmed - 1] == ' ' || body[trimmed - 1] == '\r' || body[trimmed - 1] == '\t')) --trimmed;
  const auto content = body.substr(0, trimmed);
  size_t fenceCount = 0;
  if (!content.empty() && (content[0] == '`' || content[0] == '~')) {
    while (fenceCount < content.size() && content[fenceCount] == content[0]) ++fenceCount;
  }
  if (fence_) {
    if (!content.empty() && content[0] == fence_ && fenceCount >= fenceLength_ && fenceCount == content.size()) {
      consume(text, false);
      write("</code></div>");
      fence_ = 0;
    } else {
      consume(text, true, true);
      write("<br />");
    }
    return false;
  }
  if (tableColumns_) {
    Cell cells[MAX_TABLE_COLUMNS];
    const size_t columns = splitCells(text, cells);
    if (columns && columns <= tableColumns_) {
      tableRow(text, cells, columns, false);
      return false;
    }
    closeTable();
  }
  if (fenceCount >= 3) {
    closeList();
    fence_ = content[0];
    fenceLength_ = fenceCount;
    consume(text, false);
    write("<div style=\"text-indent:0\"><code>");
    return false;
  }
  if (content.empty()) {
    closeList();
    consume(text, false);
    write("<br />");
    return false;
  }
  // ATX headings: one to six # followed by whitespace, not hashtags.
  uint8_t heading;
  const auto title = txt_chapter_index::markdownTitle(text, heading);
  if (!title.empty()) {
    closeList();
    char tag[] = "<h1>";
    tag[2] = static_cast<char>('0' + heading);
    write(tag);
    const size_t prefix = static_cast<size_t>(title.data() - text.data());
    consume(text.substr(0, prefix), false);
    inlineText(title);
    consume(text.substr(prefix + title.size()), false);
    char close[] = "</h1>";
    close[3] = tag[2];
    write(close);
    return false;
  }
  if (content[0] == '>' && (content.size() == 1 || content[1] == ' ')) {
    closeList();
    const size_t prefix = content.size() > 1 ? 2 : 1;
    write("<blockquote>");
    consume(text.substr(0, indent + prefix), false);
    inlineText(body.substr(prefix));
    write("</blockquote>");
    return false;
  }
  const char rule = content[0];
  size_t markers = 0;
  if (rule == '-' || rule == '*' || rule == '_') {
    bool all = true;
    for (char value : content) {
      if (value == rule)
        ++markers;
      else if (value != ' ' && value != '\t')
        all = false;
    }
    if (all && markers >= 3) {
      closeList();
      consume(text, false);
      write("<hr />");
      return false;
    }
  }
  size_t prefix = 0;
  char list = 0;
  if (content.size() > 1 && strchr("-*+", content[0]) && content[1] == ' ') {
    prefix = 2;
    list = 'u';
  } else {
    while (prefix < content.size() && prefix < 9 && content[prefix] >= '0' && content[prefix] <= '9') ++prefix;
    if (prefix && prefix + 1 < content.size() && (content[prefix] == '.' || content[prefix] == ')') &&
        content[prefix + 1] == ' ') {
      prefix += 2;
      list = 'o';
    }
  }
  if (list) {
    if (list_ != list) {
      closeList();
      write(list == 'o' ? "<ol>" : "<ul>");
      list_ = list;
    }
    write("<li>");
    consume(text.substr(0, indent + prefix), false);
    inlineText(body.substr(prefix));
    write("</li>");
  } else {
    closeList();
    Cell cells[MAX_TABLE_COLUMNS];
    if (length_ <= pendingCapacity_ && splitCells(text, cells)) {
      memcpy(pending_, line_, length_);
      pendingLength_ = length_;
      pendingPosition_ = position_;
      pendingHasNewline_ = false;
      return true;
    }
    write("<p>");
    inlineText(text);
    write("</p>");
  }
  return false;
}

void MarkdownFormatter::add(const uint32_t source, const uint8_t* character, const uint8_t width,
                            const uint8_t utf8Length) {
  if (!length_) position_ = source;
  if (character[0] == '\n') {
    if (processLine()) {
      pendingNewline_ = source;
      pendingHasNewline_ = true;
    } else {
      sink_.consume(sink_.context, source, character, width, utf8Length, false, false);
    }
    length_ = 0;
    return;
  }
  if (length_ + utf8Length > capacity_) {
    flushPending();
    closeTable();
    if (!overflow_ && !fence_) {
      closeList();
      write("<p>");
    }
    consume(std::string_view(reinterpret_cast<const char*>(line_), length_), true, fence_ != 0);
    length_ = 0;
    overflow_ = true;
  }
  memcpy(line_ + length_, character, utf8Length);
  length_ += utf8Length;
}

void MarkdownFormatter::finish() {
  if (length_ || overflow_) processLine();
  flushPending();
  closeTable();
  closeList();
  if (fence_) write("</code></div>");
}
