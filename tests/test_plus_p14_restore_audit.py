"""Offline transaction replay. Fake hardware cannot establish an electrical root cause."""
from pathlib import Path
import os
import re
import tempfile
import unittest

from test_battery_stability import method, run_cpp

PORT = Path(__file__).resolve().parents[1]
BOARD = PORT / 'firmware/rickyos-plus-1.2/components/metalio_board'


def without_includes(text):
    return re.sub(r'^#include .*$', '', text, flags=re.MULTILINE)


class P14RestoreAudit(unittest.TestCase):
    def replay(self, board, oem=False):
        config = (board / 'config.h').read_text()
        source = (board / 'display/epd_board_metalio_eink4_plus.c').read_text()
        local = (BOARD / 'display/epd_board_metalio_eink4_plus.c').read_text()
        defines = '\n'.join(line for line in config.splitlines()
                            if re.match(r'#define EPD_(IO|TPS_IO)_', line))
        masks = source[source.index('#define CFG_IO_BIT'):source.index('/* 并行数据')]
        functions = '\n'.join(method(source, signature) for signature in [
            'static uint8_t p0_output_value(', 'static void p0_write(', 'static void p1_write(',
            'void epd_board_metalio_eink4_plus_ioexp_mask_for_sleep(',
            'bool epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin(',
            'void epd_board_metalio_eink4_plus_sleep_cut_rails(',
            'void epd_board_metalio_eink4_plus_ioexp_restore_keys(',
            'void epd_board_metalio_eink4_plus_sleep_restore_rails('])
        if not oem:
            functions = (method(local, 'bool epd_board_metalio_eink4_plus_sleep_io_checked(') +
                         method(local, 'bool epd_board_metalio_eink4_plus_ioexp_restore_keys_checked(') + functions +
                         method(local, 'bool epd_board_metalio_eink4_plus_sleep_restore_rails_staged('))
        # Both board sequences use the CURRENT production transport below. This
        # comparison isolates board bit transitions, not OEM legacy-IDF timing.
        code = '#define WAKE_DIR 0x80\n' + r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include "pca9555.h"
using gpio_num_t=int;
#define CFG_INTR 2
int gpio_get_level(int){return 1;}
#define TAG "audit"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(unsigned){}
bool busLocked=false;
bool metalio_i2c1_lock(int){assert(!busLocked);busLocked=true;return true;}
void metalio_i2c1_unlock(){assert(busLocked);busLocked=false;}
struct Frame{uint8_t address,reg,value;bool applied;};
std::vector<Frame> frames;
uint8_t regs[8]{};int failReg=-1,failReadReg=-1,dropReg=-1;
struct FakeWire {
 uint8_t address=0,selected=0;std::vector<uint8_t> tx,rx;size_t index=0;
 bool begin(int,int,int){return true;}
 void setTimeOut(int){}
 void beginTransmission(uint8_t a){address=a;tx.clear();}
 size_t write(uint8_t b){tx.push_back(b);return 1;}
 uint8_t endTransmission(bool stop){
  assert(busLocked || tx.empty());
  if(tx.empty())return 0;
  assert(address==0x20);selected=tx[0];
  if(stop && tx.size()==2){
   assert(selected==2 || selected==3 || selected==6 || selected==7);
   const bool applied=selected!=failReg;
   frames.push_back({address,selected,tx[1],applied});
   if(!applied)return 5;
   if(selected!=dropReg)regs[selected]=tx[1];
  } else assert(tx.size()==1);
  return 0;
 }
 size_t requestFrom(uint8_t a,size_t n,bool){
  assert(busLocked && a==address);rx.clear();index=0;if(selected==failReadReg)return 0;
  for(size_t i=0;i<n;++i){
   unsigned r=(selected&~1u)|((selected+i)&1u);
   rx.push_back(r<2 ? uint8_t((regs[r+2]&~regs[r+6])|regs[r+6]) : regs[r]);
  }
  return n;
 }
 int available(){return int(rx.size()-index);}
 uint8_t read(){return rx.at(index++);}
} Wire1;
''' + defines + '\n' + masks + r'''
struct {int port=1;bool pwrup=false,vcom_ctrl=false,wakeup=false;} config_reg;
bool s_ioexp_ok=true;[[maybe_unused]] bool s_sleep_keys_restored=false;
uint8_t s_p1_out=0x14,s_motor_on=0,s_pa_on=0,s_cam_scr_on=0x20;
static uint8_t p0_output_value();
''' + without_includes((BOARD / 'bus/plus_i2c_wire.cc').read_text()).replace(
            'namespace {', '#include <atomic>\nnamespace {', 1) + without_includes(
                (BOARD / 'bus/pca9555.c').read_text()) + functions + r'''
void setup(){
 for(auto &v:regs)v=0;
 regs[6]=0xc0;regs[7]=0xe3;
 s_p1_out=0x14;s_cam_scr_on=0x20;failReg=failReadReg=dropReg=-1;frames.clear();
 p0_write();p1_write();frames.clear();
}
void park(){
 epd_board_metalio_eink4_plus_ioexp_mask_for_sleep();
 assert(epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin());
 epd_board_metalio_eink4_plus_sleep_cut_rails();
 assert(regs[2]==0 && regs[3]==0x87 && regs[6]==0 && regs[7]==WAKE_DIR);
}
void checkP14(){
 assert(frames.size()==2);
 assert(frames[0].address==0x20 && frames[0].reg==3 && frames[0].value==0x14);
 assert(frames[1].reg==2 && frames[1].value==0x20);
 assert((regs[3]&0x08)==0); // P13 shutdown never asserted by this normal sequence.
}
int main(){
 assert(plus_i2c_begin()==ESP_OK);
 setup();
 for(unsigned cycle=0;cycle<128;++cycle){
  park();epd_board_metalio_eink4_plus_ioexp_restore_keys();
  assert(regs[3]==0x04 && regs[7]==0xe3);
  frames.clear();
  epd_board_metalio_eink4_plus_sleep_restore_rails();checkP14();
  assert(regs[3]==0x14 && regs[2]==0x20);
 }
 // The void compatibility API must also refuse rails after a failed restore.
 setup();park();failReg=7;
 epd_board_metalio_eink4_plus_ioexp_restore_keys();
 assert(regs[7]==WAKE_DIR && regs[3]==0x04);
 failReg=-1;frames.clear();
'''
        code += (' epd_board_metalio_eink4_plus_sleep_restore_rails();checkP14();\n'
                 if oem else ' epd_board_metalio_eink4_plus_sleep_restore_rails();assert(frames.empty());\n')
        code += r'''
 assert(regs[7]==WAKE_DIR); // The injected failed direction write did not stick.
'''
        if not oem:
            code += r'''
 // Fixed checked path: all transaction errors block both rail writes.
 for(int reg:{2,3,6,7}){
  setup();park();failReg=reg;
  const bool keysOk=epd_board_metalio_eink4_plus_ioexp_restore_keys_checked();assert(!keysOk);
  failReg=-1;frames.clear();
  epd_board_metalio_eink4_plus_sleep_restore_rails();assert(frames.empty());
  assert(!epd_board_metalio_eink4_plus_sleep_restore_rails_staged(keysOk,nullptr));assert(frames.empty());
 }
 for(int reg:{0,1,2,4,6}){
  setup();park();failReadReg=reg;
  const bool keysOk=epd_board_metalio_eink4_plus_ioexp_restore_keys_checked();assert(!keysOk);
  failReadReg=-1;frames.clear();
  assert(!epd_board_metalio_eink4_plus_sleep_restore_rails_staged(keysOk,nullptr));assert(frames.empty());
 }
 // ACK alone is insufficient: model a direction write which did not stick.
 setup();park();dropReg=7;
 assert(!epd_board_metalio_eink4_plus_ioexp_restore_keys_checked());dropReg=-1;
 // A changed state AFTER checked restore also blocks rail restoration.
 for(unsigned reg=2;reg<8;++reg){
  setup();park();const bool keysOk=epd_board_metalio_eink4_plus_ioexp_restore_keys_checked();assert(keysOk);
  regs[reg]^=1;frames.clear();
  assert(!epd_board_metalio_eink4_plus_sleep_restore_rails_staged(keysOk,nullptr));assert(frames.empty());
  regs[reg]^=1; // Even restored register state cannot revive a failed permission.
  epd_board_metalio_eink4_plus_sleep_restore_rails();assert(frames.empty());
  assert(!epd_board_metalio_eink4_plus_sleep_restore_rails_staged(true,nullptr));assert(frames.empty());
 }
 for(unsigned n=0;n<128;++n){
  setup();park();const bool keysOk=epd_board_metalio_eink4_plus_ioexp_restore_keys_checked();assert(keysOk);
  frames.clear();assert(epd_board_metalio_eink4_plus_sleep_restore_rails_staged(keysOk,nullptr));checkP14();
 }
'''
        code += '}\n'
        with tempfile.TemporaryDirectory() as stub_dir:
            stub = Path(stub_dir)
            (stub / 'driver').mkdir()
            (stub / 'driver/i2c.h').write_text('#pragma once\nusing i2c_port_t=int;\n')
            run_cpp(code, BOARD / 'bus', stub, headers={
                'esp_err.h': '#pragma once\nusing esp_err_t=int;\n'
                             'constexpr int ESP_OK=0,ESP_ERR_TIMEOUT=1,ESP_ERR_NOT_FOUND=2,ESP_FAIL=3,'
                             'ESP_ERR_INVALID_ARG=4,ESP_ERR_INVALID_STATE=5;\n',
            })

    def test_current_board_production_transport_and_failure_window(self):
        self.replay(BOARD)

    @unittest.skipUnless(os.environ.get('RICKYOS_PLUS_OEM_ROOT'),
                         'Set RICKYOS_PLUS_OEM_ROOT to the local OEM 2.0.76 checkout')
    def test_oem_board_bits_through_same_transport(self):
        self.replay(Path(os.environ['RICKYOS_PLUS_OEM_ROOT']) /
                    'main/boards/metalio-e-ink4-plus', oem=True)


if __name__ == '__main__':
    unittest.main()
