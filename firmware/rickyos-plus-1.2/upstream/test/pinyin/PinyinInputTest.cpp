#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "PinyinDictionary.h"
#include "PinyinInput.h"

void type(PinyinInput& ime, const char* value) {
  ime.clear();
  for (const char* p = value; *p; ++p) assert(ime.append(*p));
}

int main() {
  PinyinInput ime;
  static_assert(sizeof(ime) <= 1200, "IME session must remain bounded below 1.2KB");
  assert(ime.empty() && !ime.backspace() && !ime.consume(0));
  type(ime, "NiHao");
  assert(!std::strcmp(ime.raw(), "nihao"));
  assert(!std::strcmp(ime.candidate(0)->text, "你好"));
  assert(ime.candidate(0)->consume == 5);
  assert(ime.consume(0) && ime.empty());
  type(ime, "nv");
  assert(!std::strcmp(ime.candidate(0)->text, "女"));
  type(ime, "lve");
  assert(ime.count());
  type(ime, "yuedushenghuo");
  assert(!std::strcmp(ime.candidate(0)->text, "阅读生活"));
  assert(ime.candidate(0)->consume == std::strlen("yuedushenghuo"));
  assert(ime.consume(0) && ime.empty());
  type(ime, "nihaoricky");
  assert(!std::strcmp(ime.candidate(0)->text, "你好"));
  assert(ime.consume(0));
  assert(!std::strcmp(ime.raw(), "ricky"));
  type(ime, "nihao'ricky");
  assert(!std::strcmp(ime.candidate(0)->text, "你好"));
  assert(ime.consume(0));
  assert(!std::strcmp(ime.raw(), "ricky"));
  type(ime, "nihao'");
  assert(!std::strcmp(ime.candidate(0)->text, "你好"));
  assert(ime.consume(0) && ime.empty());
  assert(!ime.append('!') && !ime.append(' '));
  type(ime, "xi'an");
  assert(ime.count() && ime.candidate(0)->consume == 5);
  // Every single-prefix candidate must respect the explicit boundary.
  size_t singles = 0;
  do {
    for (size_t i = 0; i < ime.count(); ++i) {
      const auto* item = ime.candidate(i);
      assert(item && item->text[0] && item->consume <= 5);
      if (std::strlen(item->text) == 3) {
        assert(item->consume == 3);
        ++singles;
      }
    }
  } while (ime.nextPage());
  assert(singles);
  type(ime, "shi");
  char first[PinyinInput::MAX_TEXT];
  std::strcpy(first, ime.candidate(0)->text);
  assert(ime.hasNext());
  assert(ime.nextPage() && ime.previousPage());
  assert(!std::strcmp(first, ime.candidate(0)->text));
  assert(ime.nextPage());
  char chosen[PinyinInput::MAX_TEXT];
  std::strcpy(chosen, ime.candidate(1)->text);
  assert(ime.consume(1));
  type(ime, "shi");
  assert(!std::strcmp(chosen, ime.candidate(0)->text));
  ime.resetSession();
  type(ime, "shi");
  assert(!std::strcmp(first, ime.candidate(0)->text));
  assert(ime.backspace() && !std::strcmp(ime.raw(), "sh"));
  ime.clear();
  assert(!ime.append('\''));
  assert(ime.append('x') && ime.append('\'') && !ime.append('\''));
  ime.clear();
  for (size_t i = 0; i < PinyinInput::MAX_RAW; ++i) assert(ime.append('a'));
  assert(!ime.append('a') && std::strlen(ime.raw()) == PinyinInput::MAX_RAW);
  for (size_t i = 0; i < PinyinInput::MAX_RAW; ++i) assert(ime.backspace());
  assert(ime.empty());
  const auto start = std::chrono::steady_clock::now();
  size_t keys = 0, pages = 0;
  for (const auto& syllable : ricky_pinyin_data::syllables) {
    char key[7]{};
    for (int i = 0; i < 6; ++i) {
      const uint32_t letter = (syllable.key >> ((5 - i) * 5)) & 31;
      if (letter) key[i] = static_cast<char>('a' + letter - 1);
    }
    type(ime, key);
    keys += std::strlen(key);
    assert(ime.count());
    do {
      ++pages;
      assert(pages < 10000);
      for (size_t i = 0; i < ime.count(); ++i) {
        const auto* item = ime.candidate(i);
        assert(item->consume > 0 && item->consume <= std::strlen(ime.raw()));
        assert(std::strlen(item->text) < PinyinInput::MAX_TEXT);
        assert(std::strlen(item->text) % 3 == 0);
      }
    } while (ime.nextPage());
  }
  const auto us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
  std::printf("PASS: session=%zu bytes; %zu keys, %zu candidate pages in %lld us (HOST, not panel timing)\n",
              sizeof(ime), keys, pages, static_cast<long long>(us));
}
