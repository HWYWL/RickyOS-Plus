"""Compile the actual Plus audio worker with UART/I2S/task fakes; no hardware writes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
app = root / 'firmware/rickyos-plus-1.2/components/rickyos_application/port'
sdk = root / 'firmware/rickyos-plus-1.2/upstream/freeink-sdk/libs/hardware'
with tempfile.TemporaryDirectory() as directory:
    d = Path(directory)
    (d/'driver').mkdir(); (d/'freertos').mkdir()
    text = (sdk/'AudioManager/test/host/stubs/driver/i2s_std.h').read_text()
    text = text.replace('bool auto_clear = false;', 'bool auto_clear = false; int dma_desc_num=0, dma_frame_num=0;')
    text = text.replace('*tx = &txFake; *rx = &rxFake;', 'if (tx) *tx = &txFake; else txFake.deleted=true; if (rx) *rx = &rxFake; else rxFake.deleted=true;')
    text = text.replace('c->gpio_cfg.bclk == 6', 'c->gpio_cfg.bclk == 42').replace('c->gpio_cfg.dout == 7 && c->gpio_cfg.din == 17', '((h == &txFake && c->gpio_cfg.dout == 44 && c->gpio_cfg.din == -1) || (h == &rxFake && c->gpio_cfg.dout == -1 && c->gpio_cfg.din == 45))')
    text = text.replace('*read = bytes;', '*read = bytes; incomingSlots.erase(incomingSlots.begin(),incomingSlots.begin()+bytes/4);')
    text = text.replace('inline int i2s_channel_read(', 'struct i2s_event_data_t {};\nusing i2s_isr_callback_t=bool(*)(i2s_chan_handle_t,i2s_event_data_t*,void*);\nstruct i2s_event_callbacks_t { i2s_isr_callback_t on_recv_q_ovf=nullptr; };\ninline bool failCallbacks=false, overflow=false;\ninline i2s_isr_callback_t ovf=nullptr;\ninline int i2s_channel_register_event_callback(i2s_chan_handle_t,const i2s_event_callbacks_t* cb,void*) { ovf=cb->on_recv_q_ovf; return failCallbacks ? ESP_FAIL : ESP_OK; }\ninline int i2s_channel_read(')
    text = text.replace('assert(h->enabled);\n  if (bytes > incomingSlots', 'assert(h->enabled); if(overflow) ovf(h,nullptr,nullptr);\n  if (bytes > incomingSlots')
    text += '\nconstexpr int ESP_ERR_INVALID_SIZE=-3;\n'
    (d/'driver/i2s_std.h').write_text(text)
    text = (sdk/'AudioManager/test/host/stubs/HardwareSerial.h').read_text().replace('n != 2','n != 1').replace('rx != 47 || tx != 48','rx != 38 || tx != 39').replace('void flush() {}','int available() { return 2; }\n  void flush() {}')
    (d/'HardwareSerial.h').write_text(text)
    (d/'HalAudioOutput.h').write_text('#pragma once\n#include <cstddef>\n#include <cstdint>\nnamespace HalAudioOutput { using ReadCallback=int(*)(uint8_t*,size_t); }\n')
    (d/'config.h').write_text('#define BT_AUDIO_RX_PIN 38\n#define BT_AUDIO_TX_PIN 39\n#define AUDIO_I2S_BCLK 42\n#define AUDIO_I2S_WS 43\n#define AUDIO_I2S_DOUT 44\n#define AUDIO_I2S_DIN 45\n')
    (d/'esp_attr.h').write_text('#define IRAM_ATTR\n')
    (d/'esp_log.h').write_text('#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\ninline const char* esp_err_to_name(int) { return "fake"; }\n')
    (d/'epd_board_metalio_eink4_plus.h').write_text('#pragma once\ninline bool amp=false;\ninline void epd_board_metalio_eink4_plus_pa_set(bool on) { amp=on; }\ninline bool epd_board_metalio_eink4_plus_bt_pa_resume() { return false; }\n')
    (d/'freertos/FreeRTOS.h').write_text('#pragma once\n#define pdMS_TO_TICKS(v) (v)\n#define pdPASS 1\n')
    (d/'freertos/task.h').write_text('''#pragma once
inline void (*pending)(void*)=nullptr;
inline bool failTask=false;
inline int xTaskCreate(void(*fn)(void*),const char*,int,void*,int,void*) {
 if (failTask) return 0; pending=fn; return 1;
}
inline void run() { auto fn=pending; pending=nullptr; fn(nullptr); }
inline void vTaskDelay(int) { if(pending) run(); }
inline void vTaskDelete(void*) {}
''')
    (d/'test.cpp').write_text('''#include <PlusAudio.h>
#include <driver/i2s_std.h>
#include <HardwareSerial.h>
#include <freertos/task.h>
#include <epd_board_metalio_eink4_plus.h>
#include <detail/MetalioPcm.h>
#include <cassert>
std::vector<uint8_t> captured;
bool rejectSink=false;
bool sink(const uint8_t* p,size_t n) { captured.insert(captured.end(),p,p+n); return !rejectSink; }
int position=0;
int readSplit(uint8_t* dst,size_t) {
 const int16_t data[]={1000,-1000,2000,-2000};
 if(position==8) return 0;
 *dst=reinterpret_cast<const uint8_t*>(data)[position++]; return 1;
}
int main() {
 using namespace plus_audio;
 assert(!play(readSplit,44100,1,50) && !play(readSplit,16000,3,50));
 failTask=true; assert(!preview(35)); failTask=false;
 assert(preview(35)); assert(!preview(35)); run();
 assert(txFake.deleted && !amp && writtenSlots.size()==9600);
 assert(uartCommands=="AT+RX=2\\r\\nAT+MODE=1\\r\\n");
 writtenSlots.clear(); partialWrite=true;
 assert(play(readSplit,16000,2,50)); run();
 assert(writtenSlots.size()==4);
 assert(writtenSlots[0]==freeink::metalio::outputSample(1000,50));
 assert(writtenSlots[1]==freeink::metalio::outputSample(-1000,50));
 writtenSlots.clear(); assert(preview(35)); setVolume(0); run();
 for(auto s:writtenSlots) assert(s==0); assert(!amp);
 failWrite=true; assert(preview(35)); run(); assert(txFake.deleted && !amp); failWrite=false;
 failAllocation=true; assert(preview(35)); run(); assert(!amp); failAllocation=false;
 failInitAt=initCalls+1; assert(preview(35)); run(); assert(txFake.deleted && !amp); failInitAt=0;
 uartFail=true; assert(preview(35)); run(); assert(txFake.deleted && !amp); uartFail=false;
 failEnable=true; assert(preview(35)); run(); assert(txFake.deleted && !amp); failEnable=false;
 assert(preview(35)); shutdown(); assert(!amp && !pending);
 assert(preview(35)); run(); assert(txFake.deleted && !amp);
 assert(!busy() && !failed());
 assert(!capture(nullptr));
 failTask=true; assert(!capture(sink)); assert(!busy() && failed()); failTask=false;
 incomingSlots={4096,12345,-4096,56789,INT32_MAX,0,INT32_MIN,0};
 assert(capture(sink)); assert(busy() && !preview(35) && !capture(sink)); run();
 assert(!busy() && failed() && rxFake.deleted && !amp);
 assert(captured.size()==8);
 const int16_t expected[]={1,-1,32767,-32768};
 assert(memcmp(captured.data(),expected,8)==0);
 captured.clear(); incomingSlots={4096,0}; rejectSink=true;
 assert(capture(sink)); run(); assert(failed() && captured.size()==2 && rxFake.deleted); rejectSink=false;
 failCallbacks=true; assert(capture(sink)); run(); assert(failed() && rxFake.deleted); failCallbacks=false;
 incomingSlots={4096,0}; overflow=true; captured.clear(); assert(capture(sink)); run(); assert(failed() && captured.empty() && rxFake.deleted); overflow=false;
 incomingSlots={4096}; assert(capture(sink)); run(); assert(failed() && rxFake.deleted);
 assert(capture(sink)); shutdown(); assert(!busy() && !failed() && !amp);
 failAllocation=true; assert(capture(sink)); run(); assert(failed() && !amp); failAllocation=false;
 failInitAt=initCalls+1; assert(capture(sink)); run(); assert(failed() && rxFake.deleted); failInitAt=0;
 uartFail=true; assert(capture(sink)); run(); assert(failed() && rxFake.deleted); uartFail=false;
 failEnable=true; assert(capture(sink)); run(); assert(failed() && rxFake.deleted); failEnable=false;
}
''')
    command=['c++','-std=c++20','-fsanitize=address,undefined','-g',f'-I{d}',f'-I{app}',f'-I{sdk}/BoardConfig/include',str(d/'test.cpp'),str(app/'PlusAudio.cpp'),'-o',str(d/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(d/'test')],check=True)
print('Plus audio: format/pins, preview, volume/mute, partial PCM/I2S, cancellation, failure cleanup, capture pins/left-slot conversion, exclusive ownership, sink/timeout failures and retries passed')
