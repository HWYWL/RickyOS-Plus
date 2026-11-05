"""Real diagnostic init wrapper changes only TX AMPDU and preserves caller ownership."""
from pathlib import Path
import subprocess,tempfile
source=Path('firmware/rickyos-plus-1.2/components/rickyos_application/port/PlusWifiTxConfig.cpp').resolve()
with tempfile.TemporaryDirectory() as d:
 root=Path(d)
 (root/'esp_wifi.h').write_text('#pragma once\nusing esp_err_t=int;struct wifi_init_config_t{int ampdu_tx_enable;int rx_count;void* context;};\n')
 (root/'esp_log.h').write_text('#pragma once\n#define ESP_LOGI(...) ((void)0)\n')
 (root/'test.cpp').write_text('#define RICKYOS_PLUS_RADIO_RATE 1\n#include <cassert>\n#include "'+str(source)+'"\n'+'''wifi_init_config_t seen{};bool nullSeen=false;
extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t*c){if(!c)nullSeen=true;else seen=*c;return 17;}
int main(){int token=7;wifi_init_config_t original{1,32,&token};
 assert(__wrap_esp_wifi_init(&original)==17&&seen.ampdu_tx_enable==1&&seen.rx_count==32&&seen.context==&token);
 plusRadioOverrideTxAggregation(true);assert(__wrap_esp_wifi_init(&original)==17&&seen.ampdu_tx_enable==0);
 assert(original.ampdu_tx_enable==1&&seen.rx_count==32&&seen.context==&token);
 plusRadioOverrideTxAggregation(false);assert(__wrap_esp_wifi_init(&original)==17&&seen.ampdu_tx_enable==1);
 assert(__wrap_esp_wifi_init(nullptr)==17&&nullSeen);
}
''')
 subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined','-I'+str(root),str(root/'test.cpp'),'-o',str(root/'test')],check=True)
 subprocess.run([str(root/'test')],check=True)
print('Actual Wi-Fi init override: caller unchanged, only AMPDU toggled, reset/null/error return preserved')
