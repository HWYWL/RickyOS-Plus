#include "PinyinInput.h"

#include <cstring>

#include "PinyinDictionary.h"

namespace {
using namespace ricky_pinyin_data;
struct Phrase {
  const char* key;
  const char* text;
};
// Original small vocabulary for first-use/profile/reading tasks. A prototype,
// not a statistical language model or a comprehensive phrase dictionary.
constexpr Phrase phrases[] = {
    {"nihao", "你好"},        {"zaoshanghao", "早上好"},  {"wanshanghao", "晚上好"}, {"wanan", "晚安"},
    {"xiexie", "谢谢"},       {"zaijian", "再见"},        {"yuedu", "阅读"},         {"dushu", "读书"},
    {"shuku", "书库"},        {"shenghuo", "生活"},       {"jintian", "今天"},       {"mingtian", "明天"},
    {"zuotian", "昨天"},      {"kuaile", "快乐"},         {"kaixin", "开心"},        {"anjing", "安静"},
    {"mandian", "慢点"},      {"yidian", "一点"},         {"yihui", "一会"},         {"shufang", "书房"},
    {"shijian", "时间"},      {"pengyou", "朋友"},        {"shijie", "世界"},        {"zhongguo", "中国"},
    {"zhongwen", "中文"},     {"pinyin", "拼音"},         {"shuru", "输入"},         {"mingzi", "名字"},
    {"nicheng", "昵称"},      {"xihuan", "喜欢"},         {"women", "我们"},         {"nimen", "你们"},
    {"gougou", "狗狗"},       {"shuben", "书本"},         {"yueliang", "月亮"},      {"taiyang", "太阳"},
    {"xingxing", "星星"},     {"chuntian", "春天"},       {"xiatian", "夏天"},       {"qiutian", "秋天"},
    {"dongtian", "冬天"},     {"chengshi", "城市"},       {"xiaoshuo", "小说"},      {"gushi", "故事"},
    {"xuexi", "学习"},        {"gongzuo", "工作"},        {"jiayou", "加油"},        {"manmanlai", "慢慢来"},
    {"xiaowangzi", "小王子"}, {"waerdenghu", "瓦尔登湖"},
};

const Syllable* find(const char* key, size_t n) {
  uint32_t code = 0;
  for (size_t i = 0; i < 6; ++i) code = (code << 5) | (i < n ? key[i] - 'a' + 1 : 0);
  size_t lo = 0, hi = sizeof(syllables) / sizeof(syllables[0]);
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (syllables[mid].key < code)
      lo = mid + 1;
    else if (syllables[mid].key > code)
      hi = mid;
    else
      return &syllables[mid];
  }
  return nullptr;
}

void encode(uint16_t cp, char* out) {
  out[0] = static_cast<char>(0xe0 | (cp >> 12));
  out[1] = static_cast<char>(0x80 | ((cp >> 6) & 63));
  out[2] = static_cast<char>(0x80 | (cp & 63));
  out[3] = 0;
}
}  // namespace

bool PinyinInput::append(char c) {
  if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  if ((c < 'a' || c > 'z') && c != '\'') return false;
  if (length == MAX_RAW || (c == '\'' && (!length || composition[length - 1] == '\''))) return false;
  composition[length++] = c;
  composition[length] = 0;
  skip = 0;
  refresh();
  return true;
}

bool PinyinInput::backspace() {
  if (!length) return false;
  composition[--length] = 0;
  skip = 0;
  refresh();
  return true;
}

void PinyinInput::clear() {
  length = candidateCount = 0;
  skip = 0;
  composition[0] = 0;
  more = false;
}

bool PinyinInput::consume(size_t index) {
  const auto* item = candidate(index);
  if (!item || !item->consume || item->consume > length) return false;
  const size_t used = item->consume;
  if (used < sizeof(learned[0].key) && std::strlen(item->text) < sizeof(learned[0].text)) {
    auto& entry = learned[learnedNext++ % 16];
    std::memcpy(entry.key, composition, used);
    entry.key[used] = 0;
    std::strcpy(entry.text, item->text);
  }
  std::memmove(composition, composition + used, length - used + 1);
  length -= used;
  skip = 0;
  refresh();
  return true;
}

void PinyinInput::resetSession() {
  clear();
  std::memset(learned, 0, sizeof(learned));
  learnedNext = 0;
}

void PinyinInput::compose() {
  best[0] = 0;
  for (size_t i = 0; i <= length; ++i) scores[i] = -30000;
  scores[0] = 0;
  constexpr uint16_t PHRASE_FLAG = 0x8000;
  for (size_t at = 0; at < length; ++at) {
    if (scores[at] == -30000) continue;
    auto extend = [&](size_t n, uint16_t token, int bonus) {
      const size_t end = at + n;
      const int value = scores[at] + static_cast<int>(n) * 8 + bonus;
      if (value <= scores[end]) return;
      scores[end] = static_cast<int16_t>(value);
      previous[end] = static_cast<uint8_t>(at);
      tokens[end] = token;
    };
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); ++i) {
      const size_t n = std::strlen(phrases[i].key);
      if (n <= length - at && std::strncmp(composition + at, phrases[i].key, n) == 0)
        extend(n + (composition[at + n] == '\'' ? 1 : 0), PHRASE_FLAG | i, 12);
    }
    const char* separator = std::strchr(composition + at, '\'');
    const size_t end = separator ? static_cast<size_t>(separator - composition) : length;
    for (size_t n = 1; n <= 6 && at + n <= end; ++n) {
      if (separator && at + n != end) continue;
      const auto* row = find(composition + at, n);
      if (row && row->count) extend(n + (separator ? 1 : 0), static_cast<uint16_t>(row - syllables), -8);
    }
  }
  if (scores[length] == -30000) return;
  size_t bytes = 0;
  for (size_t at = length; at; at = previous[at])
    bytes += tokens[at] & PHRASE_FLAG ? std::strlen(phrases[tokens[at] & ~PHRASE_FLAG].text) : 3;
  if (bytes >= sizeof(best)) return;
  best[bytes] = 0;
  for (size_t at = length; at; at = previous[at]) {
    const uint16_t token = tokens[at];
    if (token & PHRASE_FLAG) {
      const char* word = phrases[token & ~PHRASE_FLAG].text;
      const size_t n = std::strlen(word);
      bytes -= n;
      std::memcpy(best + bytes, word, n);
    } else {
      char glyph[4];
      encode(characters[syllables[token].offset], glyph);
      bytes -= 3;
      std::memcpy(best + bytes, glyph, 3);
    }
  }
}

bool PinyinInput::nextPage() {
  if (!more) return false;
  skip += PAGE_SIZE;
  refresh();
  return true;
}

bool PinyinInput::previousPage() {
  if (!skip) return false;
  skip -= PAGE_SIZE;
  refresh();
  return true;
}

void PinyinInput::refresh() {
  candidateCount = 0;
  more = false;
  if (!length) return;
  // Remove separators for phrase matching, but retain byte offsets and reject
  // a phrase whose syllable boundaries contradict explicit apostrophes.
  // Phrases are only separator-free in this first decoder; explicit input
  // takes the unambiguous single-syllable path instead.
  compose();
  const Learned* preferred = nullptr;
  for (size_t step = 0; step < 16; ++step) {
    const auto& entry = learned[(learnedNext + 15 - step) % 16];
    if (entry.key[0] && std::strcmp(entry.key, composition) == 0) {
      preferred = &entry;
      break;
    }
  }
  size_t seen = 0;
  bool preferredAdded = false;
  bool bestAdded = false;
  auto add = [&](const char* value, size_t used) {
    if (preferredAdded && used == length && std::strcmp(value, preferred->text) == 0) return false;
    if (bestAdded && used == length && std::strcmp(value, best) == 0) return false;
    if (seen++ < skip) return false;
    if (candidateCount == PAGE_SIZE) {
      more = true;
      return true;
    }
    auto& item = page[candidateCount++];
    std::strncpy(item.text, value, MAX_TEXT - 1);
    item.text[MAX_TEXT - 1] = 0;
    item.consume = static_cast<uint8_t>(used);
    return false;
  };
  if (preferred) {
    if (add(preferred->text, length)) return;
    preferredAdded = true;
  }
  // Exact dictionary words precede synthesized sentences. Unknown phrases
  // remain selectable by syllable; one bad spelling never consumes its suffix.
  for (const auto& word : phrases)
    if (std::strlen(word.key) == length && std::strcmp(composition, word.key) == 0)
      if (add(word.text, length)) return;
  if (best[0]) {
    bool exact = false;
    for (const auto& word : phrases)
      if (std::strcmp(composition, word.key) == 0 && std::strcmp(best, word.text) == 0) exact = true;
    if (!exact && add(best, length)) return;
    bestAdded = true;
  }
  // Longest matching word first; selecting a prefix retains the rest verbatim.
  for (size_t n = length - 1; n > 0; --n) {
    for (const auto& word : phrases)
      if (std::strlen(word.key) == n && std::strncmp(composition, word.key, n) == 0)
        if (add(word.text, n + (composition[n] == '\'' ? 1 : 0))) return;
  }
  const char* separator = std::strchr(composition, '\'');
  const size_t first = separator ? static_cast<size_t>(separator - composition) : length;
  for (size_t n = first < 6 ? first : 6; n > 0; --n) {
    // Explicit separator forces a syllable, e.g. xi'an never becomes xian.
    if (separator && n != first) continue;
    const auto* row = find(composition, n);
    if (!row) continue;
    const size_t used = n + (separator && n == first ? 1 : 0);
    char glyph[4];
    for (size_t i = 0; i < row->count; ++i) {
      encode(characters[row->offset + i], glyph);
      if (add(glyph, used)) return;
    }
  }
}
