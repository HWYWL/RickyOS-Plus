"""The real RickyOtaManifest parser built for the Plus: its own board and chip, Read Pico
offers and anything outside the shared key set or slot rule refused."""
from pathlib import Path
import subprocess, tempfile
upstream = Path('firmware/rickyos-plus-1.2/upstream/src/network').resolve()
code = r'''
#include <cassert>
#include <cstring>
#include "RickyOtaManifest.h"
static bool parse(const char* json, bool& update) {
  RickyOtaManifest m;
  if (!m.feed(reinterpret_cast<const uint8_t*>(json), strlen(json))) return false;
  update = m.hasUpdate();
  return m.valid();
}
#define OFFER(board, chip, bytes) \
  "{\"schema\":1,\"product\":\"RickyOS\",\"board\":\"" board "\",\"channel\":\"stable\",\"status\":\"update_available\"," \
  "\"version\":\"1.2.0\",\"file\":\"firmware/RickyOS-Plus-1.2.0.bin\",\"bytes\":" bytes "," \
  "\"sha256\":\"" "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" "\",\"approved\":true," \
  "\"hardwareAccepted\":true,\"chipId\":" chip ",\"flashBytes\":16777216}"
int main() {
  bool update = false;
  assert(parse(OFFER("metalio-plus-s31", "32", "4823072"), update) && update);
  assert(!parse(OFFER("readpico", "9", "4823072"), update));          // the Read Pico's offer
  assert(!parse(OFFER("metalio-plus-s31", "9", "4823072"), update));  // wrong chip
  assert(!parse(OFFER("metalio-plus-s31", "32", "6029316"), update)); // over the 6.25 MiB - 512 KiB rule
  assert(parse(OFFER("metalio-plus-s31", "32", "6029312"), update));  // exactly at it
  assert(parse("{\"schema\":1,\"product\":\"RickyOS\",\"board\":\"metalio-plus-s31\",\"channel\":\"stable\",\"status\":\"no_update\"}", update) && !update);
  assert(!parse("{\"schema\":1,\"product\":\"RickyOS\",\"board\":\"metalio-plus-s31\",\"channel\":\"stable\",\"status\":\"no_update\",\"extra\":1}", update));
  static_assert(ricky_ota::CHIP_ID == 32);
  assert(strcmp(ricky_ota::COS_MANIFEST_URL, RICKYOS_COS_BASE "/ota/plus/ota.json") == 0);
  assert(strcmp(ricky_ota::MANIFEST_URL, "https://chinoryunqin.github.io/RickyOS-site/plus/ota.json") == 0);
  return 0;
}
'''
with tempfile.TemporaryDirectory() as folder:
    root = Path(folder); (root / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-DFREEINK_DEVICE_METALIO_PLUS=1', '-I', str(upstream), str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
print('Plus OTA manifest: own board and chip accepted, Read Pico offers, wrong chip, oversize and unknown keys refused (ASan/UBSan)')
