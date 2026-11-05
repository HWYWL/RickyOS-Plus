"""Exercise the actual Plus STA adapter and reject stale generated build configs."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = root / 'firmware/rickyos-plus-1.2/components/rickyos_application/port/PlusWifiSecurity.cpp'
spec = importlib.util.spec_from_file_location('security_check', root / 'scripts/check_plus_wifi_security.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    definitions = ('CONFIG_ESP_WIFI_ENABLE_WPA3_SAE', 'CONFIG_ESP_WIFI_ENABLE_SAE_H2E',
                   'CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT')
    config = temp / 'sdkconfig.h'
    good = ''.join('#define ' + key + ' 1\n' for key in definitions)
    config.write_text(good)
    checker.check(config)
    for key in definitions:
        for bad in ('', '#define ' + key + ' 0\n'):
            config.write_text(good.replace('#define ' + key + ' 1\n', bad))
            try:
                checker.check(config)
            except SystemExit as error:
                assert key in str(error)
            else:
                raise AssertionError('Stale build accepted: ' + key)
    config.write_text(good)
    (temp / 'esp_wifi.h').write_text('''#pragma once
#include <cstdint>
using esp_err_t = int;
enum wifi_interface_t { WIFI_IF_STA, WIFI_IF_AP };
enum wifi_sae_pwe_method_t { WPA3_SAE_PWE_UNSPECIFIED, WPA3_SAE_PWE_HUNT_AND_PECK,
  WPA3_SAE_PWE_HASH_TO_ELEMENT, WPA3_SAE_PWE_BOTH };
struct pmf_t { bool capable, required; };
struct sta_t { uint8_t ssid[32], password[64]; pmf_t pmf_cfg;
  wifi_sae_pwe_method_t sae_pwe_h2e; int auth_threshold; uint8_t opaque[128]; };
union wifi_config_t { sta_t sta; uint8_t ap[sizeof(sta_t)]; };
''')
    (temp / 'test.cpp').write_text('''#include <cassert>
#include <cstring>
#include "esp_wifi.h"
extern "C" esp_err_t __wrap_esp_wifi_set_config(wifi_interface_t, wifi_config_t*);
wifi_config_t seen{}; wifi_config_t* pointer = nullptr;
wifi_interface_t seenInterface = WIFI_IF_AP; int calls = 0;
extern "C" esp_err_t __real_esp_wifi_set_config(wifi_interface_t i, wifi_config_t* c) {
  ++calls; seenInterface = i; pointer = c; if (c) seen = *c; return -19;
}
int main() {
  wifi_config_t original{};
  std::memset(&original, 0x5a, sizeof(original));
  original.sta.password[0] = 'p';
  original.sta.sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED;
  original.sta.pmf_cfg = {false, false};
  original.sta.auth_threshold = 7;
  const wifi_config_t before = original;
  assert(__wrap_esp_wifi_set_config(WIFI_IF_STA, &original) == -19);
  assert(calls == 1 && seenInterface == WIFI_IF_STA && pointer != &original);
  assert(std::memcmp(&original, &before, sizeof(original)) == 0);
  wifi_config_t expected = before;
  expected.sta.pmf_cfg.capable = true;
  expected.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  assert(std::memcmp(&seen, &expected, sizeof(seen)) == 0);
  for (auto method : {WPA3_SAE_PWE_HUNT_AND_PECK, WPA3_SAE_PWE_HASH_TO_ELEMENT, WPA3_SAE_PWE_BOTH}) {
    original.sta.sae_pwe_h2e = method;
    original.sta.pmf_cfg.required = true;
    __wrap_esp_wifi_set_config(WIFI_IF_STA, &original);
    assert(seen.sta.sae_pwe_h2e == method && seen.sta.pmf_cfg.required);
    assert(!original.sta.pmf_cfg.capable && original.sta.auth_threshold == 7);
  }
  __wrap_esp_wifi_set_config(WIFI_IF_AP, &original);
  assert(pointer == &original && seenInterface == WIFI_IF_AP);
  original.sta.password[0] = 0;
  __wrap_esp_wifi_set_config(WIFI_IF_STA, &original);
  assert(pointer == &original && !seen.sta.pmf_cfg.capable);
  assert(__wrap_esp_wifi_set_config(WIFI_IF_STA, nullptr) == -19 && pointer == nullptr);
  assert(calls == 7);
}
'''.replace('#include <cstring>', '#include <cstring>\n#include <initializer_list>'))
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(temp), str(source),
                    str(temp / 'test.cpp'), '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], check=True)

print('Plus WPA3: SAE methods, PMF/auth preservation, AP/open/null pass-through, '
      'caller ownership, driver errors and stale-build rejection passed')
