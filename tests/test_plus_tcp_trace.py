"""Exercise real header-only link wrappers, preserving pbuf ownership and return codes."""
from pathlib import Path
import subprocess,tempfile
source=Path('firmware/rickyos-plus-1.2/components/rickyos_application/port/PlusTcpTrace.cpp').resolve()
with tempfile.TemporaryDirectory() as folder:
 root=Path(folder);(root/'lwip').mkdir();(root/'netif').mkdir()
 (root/'esp_log.h').write_text('#pragma once\nextern int logs;\ninline unsigned esp_log_timestamp(){return 123;}\n#define ESP_LOGI(...) (++logs)\n')
 (root/'lwip/pbuf.h').write_text('#pragma once\n#include <cstdint>\n#include <cstring>\nusing u8_t=uint8_t;using u16_t=uint16_t;using err_t=int;constexpr int ERR_OK=0;\nstruct pbuf{const uint8_t* bytes;uint16_t size;};\ninline uint16_t pbuf_copy_partial(pbuf*p,void*out,uint16_t n,uint16_t at){if(at>=p->size)return 0;n=n<p->size-at?n:p->size-at;memcpy(out,p->bytes+at,n);return n;}\n')
 (root/'lwip/ip4.h').write_text('#pragma once\n#include "pbuf.h"\nstruct ip4_addr_t{};struct netif{char name[2];uint8_t num;};\n')
 (root/'netif/ethernet.h').write_text('#pragma once\n#include <lwip/ip4.h>\nstruct eth_addr{};\n')
 code='#include <cassert>\n#include <cstdint>\nint logs=0;\n#define RICKYOS_PLUS_TCP_TRACE 1\n#include "' + str(source) + '"\nint ipCalls=0, txCalls=0,rxCalls=0;\nextern "C" {\nerr_t __real_ip4_output_if(pbuf*,const ip4_addr_t*,const ip4_addr_t*,u8_t,u8_t,u8_t,netif*){++ipCalls;return -5;}\nerr_t __real_ethernet_output(netif*,pbuf*,const eth_addr*,const eth_addr*,u16_t){++txCalls;return -6;}\nerr_t __real_ethernet_input(pbuf*)=delete;\n}\n'
 # The real receive function has the original two-argument signature.
 code=code.replace('err_t __real_ethernet_input(pbuf*)=delete;','err_t __real_ethernet_input(pbuf*,netif*){++rxCalls;return -7;}')
 code+=r'''
int main(){
 uint8_t h[64]{};h[0]=0x12;h[1]=0x34;h[2]=1;h[3]=0xbb;h[13]=2;pbuf p{h,20};
 assert(__wrap_ip4_output_if(&p,nullptr,nullptr,64,0,6,nullptr)==-5);
 assert(ipCalls==1 && logs==2);
 h[13]=0x10;logs=0;tcp("ignore-ACK",&p,0,false,nullptr,0);assert(logs==0);
 h[13]=2;p.size=13;tcp("short",&p,0,false,nullptr,0);assert(logs==0);
 uint8_t frame[64]{};frame[12]=8;frame[14]=0x45;frame[23]=6;
 frame[34]=1;frame[35]=0xbb;frame[36]=0x12;frame[37]=0x34;frame[47]=0x12;
 pbuf incoming{frame,54};
 assert(__wrap_ethernet_input(&incoming,nullptr)==-7 && rxCalls==1 && logs==1);
 logs=0;frame[14]=0x4f;assert(__wrap_ethernet_input(&incoming,nullptr)==-7 && rxCalls==2 && logs==0);
 uint8_t arpBytes[28]{};arpBytes[2]=8;arpBytes[4]=6;arpBytes[5]=4;arpBytes[7]=1;
 pbuf arpPacket{arpBytes,28};
 assert(__wrap_ethernet_output(nullptr,&arpPacket,nullptr,nullptr,0x0806)==-6 && txCalls==1 && logs==2);
}
'''
 (root/'esp_private').mkdir();(root/'freertos').mkdir()
 (root/'esp_timer.h').write_text('#pragma once\n#include <cstdint>\ninline int64_t esp_timer_get_time(){return 123000;}\n')
 (root/'esp_private/wifi.h').write_text('#pragma once\n#include <cstdint>\nusing esp_err_t=int;constexpr int ESP_OK=0;using wifi_tx_done_cb_t=void(*)(uint8_t,uint8_t*,uint16_t*,bool);inline wifi_tx_done_cb_t installed=nullptr;inline int esp_wifi_set_tx_done_cb(wifi_tx_done_cb_t cb){installed=cb;return 0;}\n')
 (root/'esp_wifi.h').write_text('#pragma once\n#include <esp_private/wifi.h>\nconstexpr int WIFI_IF_STA=0;struct wifi_ap_record_t{uint8_t bssid[6];};inline int esp_wifi_get_mac(int,uint8_t*p){for(int i=0;i<6;++i)p[i]=i+1;return 0;}inline int esp_wifi_sta_get_ap_info(wifi_ap_record_t*p){for(int i=0;i<6;++i)p->bssid[i]=i+7;return 0;}\n')
 (root/'freertos/FreeRTOS.h').write_text('#pragma once\n')
 (root/'freertos/portmacro.h').write_text('#pragma once\nusing portMUX_TYPE=int;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define portENTER_CRITICAL(x) ((void)(x))\n#define portEXIT_CRITICAL(x) ((void)(x))\n')
 code=code.replace('uint8_t h[64]{};', 'plusTcpTraceBegin();assert(installed!=nullptr);uint8_t unknown[4]{};uint16_t size=4;for(int i=0;i<70;++i)installed(0,unknown,&size,i%2==0);assert(total==70 && successful==35 && failed==35 && recorded==48);plusTcpTraceEnd();installed(0,unknown,&size,true);assert(total==70);plusTcpTraceBegin();uint8_t prefixed[72]{};prefixed[12]=0x45;prefixed[14]=0;prefixed[15]=40;prefixed[21]=6;prefixed[32]=0x12;prefixed[33]=0x34;prefixed[34]=1;prefixed[35]=0xbb;prefixed[45]=2;size=72;installed(0,prefixed,&size,false);assert(recorded==1 && samples[0].source==0x1234 && samples[0].destination==443 && samples[0].format==15 && failed==1);plusTcpTraceEnd();logs=0;uint8_t h[64]{};')
 code=code.replace('logs=0;uint8_t h[64]{};', 'plusTcpTraceBegin();uint8_t frame72[72]{};frame72[0]=0x48;frame72[1]=0x41;for(int i=0;i<6;++i){frame72[4+i]=i+7;frame72[10+i]=i+1;}size=72;installed(0,frame72,&size,false);assert(recorded==1&&samples[0].wifiType==2&&samples[0].wifiSubtype==4&&samples[0].protectedFrame==1&&samples[0].interface==0);frame72[4]=99;installed(1,frame72,&size,true);assert(samples[1].wifiType==255&&samples[1].interface==1);plusTcpTraceEnd();logs=0;uint8_t h[64]{};')
 (root/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++20','-Wno-unused-parameter','-fsanitize=address,undefined','-I'+str(root),str(root/'test.cpp'),'-o',str(root/'test')],check=True)
 subprocess.run([str(root/'test')],check=True)
print('Actual TCP trace wrappers: SYN/SYNACK/ARP filters, truncated packets, ACK exclusion and original return codes passed')
