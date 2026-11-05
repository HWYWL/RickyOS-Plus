"""Execute the linked SDK's PSRAM resume wait; no device or SDK edits.

Set RICKYOS_IDF_PATH to the SDK used for the Plus build. This checks software
ordering / tick arithmetic, not physical PSRAM retention or wake latency.
"""
from pathlib import Path
import os
import unittest

from test_battery_stability import method, run_cpp

PORT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get('RICKYOS_IDF_PATH', str(PORT / 'tools/esp-idf-v6.1')))
RESUME = SDK / 'components/esp_psram/device/esp_psram_impl_ap_oct.c'
RTC = SDK / 'components/esp_hw_support/port/esp32s31/rtc_time.c'


class PsramResumeTest(unittest.TestCase):
    @unittest.skipUnless(RESUME.is_file() and RTC.is_file(), 'set RICKYOS_IDF_PATH to the actual Plus SDK')
    def test_resume_wait_and_mr4_restore_before_return(self):
        code = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#define CONFIG_PM_SLP_SPIRAM_HALFSLEEP_EXIT_WAIT_DELAY 150
#define RTC_CLK_CAL_FRACT 19
constexpr int PSRAM_CTRLR_LL_MSPI_ID_3=3,AP_OCT_PSRAM_REG_WRITE=0xc0;
constexpr int AP_OCT_PSRAM_WR_CMD_BITLEN=16,AP_OCT_PSRAM_ADDR_BITLEN=32;
struct {uint8_t mr4;uint64_t halfsleep_wakeup_tick;}s_halfsleep_ctx;
uint64_t tick=0,restoredAt=0;unsigned polls=0,restores=0;
uint64_t rtc_time_get(){++polls;return tick++;}
void psram_ctrlr_ll_common_transaction(int id,int cmd,int cmdBits,int reg,int addrBits,int dummy,
 const uint8_t* data,int dataBits,void* out,int outBits,bool mode){
 assert(id==3 && cmd==AP_OCT_PSRAM_REG_WRITE && cmdBits==16 && reg==4 && addrBits==32);
 assert(!dummy && data==&s_halfsleep_ctx.mr4 && dataBits==16 && !out && !outBits && !mode);
 restoredAt=tick-1;++restores;
}
'''
        code += method(RTC.read_text(), 'uint64_t rtc_time_us_to_slowclk(')
        code += method(RESUME.read_text(), 'void esp_psram_impl_resume_from_halfsleep_mode(')
        code += r'''
int main(){
 for(uint32_t period:{1u<<19,7u<<19,16u<<19,16000000u}){
  for(uint64_t elapsed:{0u,1u,149u,150u,151u,2000u}){
   for(uint8_t mr4:{uint8_t(0),uint8_t(0x55),uint8_t(0xff)}){
    s_halfsleep_ctx={mr4,1000000};polls=restores=0;
    const uint64_t wait=rtc_time_us_to_slowclk(150,period);
    const uint64_t deadline=s_halfsleep_ctx.halfsleep_wakeup_tick+wait;
    tick=s_halfsleep_ctx.halfsleep_wakeup_tick+elapsed;
    esp_psram_impl_resume_from_halfsleep_mode(period);
    assert(restores==1 && restoredAt>=deadline && polls<=wait+1);
    if(elapsed>=wait)assert(polls==1); // Existing wake work consumes the wait budget.
    assert(s_halfsleep_ctx.mr4==mr4);
   }
  }
 }
}
'''
        run_cpp(code)


if __name__ == '__main__':
    unittest.main()
