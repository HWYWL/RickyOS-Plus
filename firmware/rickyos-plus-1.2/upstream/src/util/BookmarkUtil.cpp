#include "BookmarkUtil.h"

#include <algorithm>
#include <string>

std::string BookmarkUtil::getBookmarksDir() { return "/.crosspoint/bookmarks/"; }

std::string BookmarkUtil::getBookmarkPath(const std::string& bookPath) {
  // remove leading slash and replace internal slashes to create a flat filename
  std::string bookName = std::string(bookPath).erase(0, 1);
  std::replace(bookName.begin(), bookName.end(), '/', '_');
  std::replace(bookName.begin(), bookName.end(), '\\', '_');
  const size_t lastDot = bookName.find_last_of('.');
  if (lastDot != std::string::npos) {
    bookName.erase(lastDot);
  }
  bookName += ".json";
  return getBookmarksDir() + bookName;
}

std::string BookmarkUtil::sanitizeBookmarkSummary(std::string summary) {
  summary.erase(
      std::unique(summary.begin(), summary.end(), [](char a, char b) { return std::isspace(a) && std::isspace(b); }),
      summary.end());
  summary.erase(std::remove(summary.begin(), summary.end(), '\n'), summary.end());
  summary.erase(summary.begin(),
                std::find_if(summary.begin(), summary.end(), [](unsigned char ch) { return !std::isspace(ch); }));
  summary.erase(
      std::find_if(summary.rbegin(), summary.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
      summary.end());
  // Page text joins every word with a space, and CJK text is one word per character:
  // drop a single space whose neighbours are both non-ASCII ("第 一 章" -> "第一章").
  std::string joined;
  joined.reserve(summary.size());
  for (size_t i = 0; i < summary.size(); ++i) {
    const bool between = summary[i] == ' ' && i > 0 && i + 1 < summary.size() &&
                         (static_cast<unsigned char>(summary[i - 1]) & 0x80) &&
                         (static_cast<unsigned char>(summary[i + 1]) & 0x80);
    if (!between) joined += summary[i];
  }
  summary.swap(joined);
  if (summary.size() > 72) {
    // Cut on a character boundary: back off over UTF-8 continuation bytes.
    size_t cut = 72;
    while (cut > 0 && (static_cast<unsigned char>(summary[cut]) & 0xC0) == 0x80) --cut;
    summary.resize(cut);
  }
  return summary;
}
