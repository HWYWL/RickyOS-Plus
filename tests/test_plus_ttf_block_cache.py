"""Plus streamed-TTF block cache: prefixRead() returns exactly the file's bytes, with fewer card reads.

Extracts the real prefixRead()/readStreamTail(), backs HalFile with an in-memory file, and
compares every read against the file over random offsets/lengths (prefix straddles, block
boundaries, end of file, seek probes, bulk reads) and a failed block-cache allocation (ASan/UBSan).
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / 'firmware/rickyos-plus-1.2/upstream/src/SdCardFontSystem.cpp').read_text()


def extract(signature):
    start = SRC.index(signature)
    brace = SRC.index('{', start)
    depth = 0
    for i in range(brace, len(SRC)):
        depth += {'{': 1, '}': -1}.get(SRC[i], 0)
        if depth == 0:
            return SRC[start:i + 1]
    raise AssertionError(signature)


methods = extract('unsigned long SdCardFontSystem::prefixRead(') + '\n' + \
    extract('unsigned long SdCardFontSystem::readStreamTail(')

program = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define FREEINK_DEVICE_METALIO_PLUS 1
#define LOG_ERR(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
static bool g_failAlloc = false;
static void* heap_caps_malloc(size_t n, int) { return g_failAlloc ? nullptr : malloc(n); }
static std::vector<uint8_t> g_file;
static unsigned long g_cardReads = 0;
struct HalFile {};
struct SdCardFontRegistry {
  static unsigned long halFileRead(void*, unsigned long offset, unsigned char* buffer, unsigned long count) {
    if (count == 0) return 0;
    ++g_cardReads;
    if (offset >= g_file.size()) return 0;
    const unsigned long n = count < g_file.size() - offset ? count : g_file.size() - offset;
    memcpy(buffer, g_file.data() + offset, n);
    return n;
  }
};
class SdCardFontSystem {
 public:
  struct TtfSource {
    std::vector<uint8_t> bytes;
    HalFile file;
    unsigned long size = 0;
    static constexpr unsigned long kBlockBytes = 4096;
    static constexpr int kBlockCount = 32;
    uint8_t* blockData = nullptr;
    unsigned long blockStart[kBlockCount] = {};
    unsigned long blockLen[kBlockCount] = {};
    uint32_t blockUse[kBlockCount] = {};
    uint32_t blockClock = 0;
    bool blockAllocFailed = false;
  };
  static unsigned long prefixRead(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);
  static unsigned long readStreamTail(TtfSource* s, unsigned long offset, unsigned char* buffer, unsigned long count);
};
''' + methods + r'''
static unsigned long run(bool failAlloc, unsigned seed) {
  srand(seed);
  g_failAlloc = failAlloc;
  g_file.resize(300000 + rand() % 5000);  // not a multiple of the block size
  for (auto& b : g_file) b = rand() & 0xFF;
  SdCardFontSystem::TtfSource s;
  s.size = g_file.size();
  s.bytes.assign(g_file.begin(), g_file.begin() + 65536 + rand() % 999);  // odd prefix length
  g_cardReads = 0;
  std::vector<uint8_t> out(20000);
  for (int i = 0; i < 60000; ++i) {
    unsigned long offset, count;
    switch (rand() % 6) {
      case 0: offset = s.bytes.size() - rand() % 300; count = rand() % 900; break;    // prefix straddle
      case 1: offset = s.size - rand() % 600; count = rand() % 900; break;            // end of file
      case 2: offset = rand() % s.size; count = 0; break;                             // seek probe
      case 3: offset = rand() % s.size; count = 4097 + rand() % 12000; break;         // bulk read
      case 4: offset = (rand() % 70) * 4096 + 4090 + rand() % 12; count = rand() % 64; break;  // block edge
      default: offset = s.bytes.size() + rand() % 40000; count = 1 + rand() % 400; break;   // glyph-sized
    }
    const unsigned long got = SdCardFontSystem::prefixRead(&s, offset, out.data(), count);
    unsigned long expect = 0;
    if (count != 0 && offset < s.size) expect = count < s.size - offset ? count : s.size - offset;
    assert(got == expect);
    assert(got == 0 || memcmp(out.data(), g_file.data() + offset, got) == 0);
  }
  free(s.blockData);
  return g_cardReads;
}
int main() {
  const unsigned long cached = run(false, 1), direct = run(true, 1);
  assert(cached < direct);
  printf("Plus TTF block cache: 60000 reads (prefix straddles, block edges, end of file, seek probes, bulk) "
         "return the file's exact bytes; card reads %lu with the cache vs %lu through (ASan/UBSan)\n", cached, direct);
}
'''

with tempfile.TemporaryDirectory(prefix='plus-ttf-block-') as d:
    d = Path(d)
    (d / 'test.cpp').write_text(program)
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-g', str(d / 'test.cpp'), '-o', str(d / 'test')], check=True)
    subprocess.run([str(d / 'test')], check=True)
