"""Opt-in motion-only scan deadlines; do not alter line clocks/LUTs/UI/defaults."""
from pathlib import Path
import subprocess,tempfile
PORT=Path(__file__).resolve().parents[1]
LCD=PORT/'firmware/rickyos-plus-1.2/components/metalio_board/display/output_lcd'
s=(LCD/'phase_lookahead.inc').read_text();a=s.index('#ifndef RICKYOS_PLUS_READER_MOTION_PERIOD_US');b=s.index('static bool wait_phase_feeders(')
gate=s[a:b]
s=(LCD/'render_lcd.c').read_text();a=s.index('static void pace_phase_min(');b=s.index('#if RICKYOS_PLUS_PHASE_LOOKAHEAD',a)
pacer=s[a:b]
with tempfile.TemporaryDirectory(prefix='plus-motion-cadence-')as d:
 p=Path(d)
 (p/'test.c').write_text(r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#define EPD_MIN_PHASE_US 12000
#define CONFIG_IDF_TARGET_ESP32S31 1
#ifndef RICKYOS_PLUS_READER_MOTION_PERIOD_US
#define RICKYOS_PLUS_READER_MOTION_PERIOD_US 0
#endif
typedef struct{int turn_phases,cycle_frames;bool reader_text_finish;struct{int spread;}turn_region;}RenderContext_t;
static int64_t now;static int yields;
int64_t esp_timer_get_time(void){return now;}
void vTaskDelay(int n){assert(!n);now+=100;++yields;}
"""+gate+pacer+r"""
int main(void){
 for(int phases=0;phases<=48;phases+=48)for(int more=0;more<=23;more+=23)
 for(int ui=0;ui<=8;ui+=8)for(int text=0;text<2;text++){
  RenderContext_t c={.turn_phases=phases,.cycle_frames=phases+more,.reader_text_finish=text,.turn_region={ui}};
  int expected=phases&&more&&!ui&&text?RICKYOS_PLUS_READER_MOTION_PERIOD_US:0;
  assert(reader_motion_period_us(&c)==expected);
 }
 now=1000;yields=0;pace_phase_min(1000,20000);assert(now==21000&&yields==200);
 yields=0;pace_phase_min(0,20000);assert(yields==0); // Already late: no negative delay/catch-up.
 now=0;pace_phase(0);assert(now==12000);
}
""")
 common=['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')]
 for period in [0,12000,20000,25000]:
  subprocess.run([*common,f'-DRICKYOS_PLUS_READER_MOTION_PERIOD_US={period}'],check=True)
  subprocess.run([str(p/'test')],check=True)
 for period in [11999,25001,-1]:
  result=subprocess.run([*common,f'-DRICKYOS_PLUS_READER_MOTION_PERIOD_US={period}'],capture_output=True,text=True)
  assert result.returncode and 'Reader motion period must be' in result.stderr
print('Motion cadence: opt-in0/12000..25000 bounds, reader motion-only gate, UI/off/paper unchanged, final settle and expired deadline passed (ASan/UBSan)')
