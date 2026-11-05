"""Reproduce q10 erase-order regression on actual native LUT preparation.

For binary text, shared and departing old-black strokes must erase together
before the first new-black write. Phase8 overlap broke this accepted q6 order.
This is an action-order guard, not an optical simulator or a no-ghosting claim.
"""
from pathlib import Path
import subprocess,tempfile
PORT=Path(__file__).resolve().parents[1]
DISPLAY=PORT/'firmware/rickyos-plus-1.2/components/metalio_board/display'
with tempfile.TemporaryDirectory(prefix='plus-white-preimage-') as d:
 p=Path(d);(p/'esp_log.h').write_text('#define ESP_LOGD(...) ((void)0)\n')
 (p/'probe.c').write_text('#define main previous_tests\n#include "'+str(PORT/'tests/native_turn_host.c')+'"\n#undef main\n'+r'''
int main(void) {
 my_waveform_init(); real_wave=true;
 const EpdWaveformPhases* wave=NULL;
 for(int i=0;i<MY_WAVEFORM.num_modes;i++)if(MY_WAVEFORM.mode_data[i]->type==MY_MODE_READER_TURN)
  wave=MY_WAVEFORM.mode_data[i]->range_data[0];
 assert(wave&&wave->phases==48);
 int mismatch=0;
 for(int style=0;style<=3;style++)for(int orientation=0;orientation<4;orientation++) {
  RenderContext_t c=context(256,16);c.reader_text_finish=true;allocations=fail_at=0;
  ricky_native_begin(&c,wave,(RickyNativeTurn){.style=style,.orientation=orientation});
  assert(c.turn_phases==48);
  int firstBlack=0;
  while(firstBlack<48&&c.turn_luts[firstBlack*1024+15*4]!=1)++firstBlack;
  assert(firstBlack==28);
  int differences=0;
  for(int tick=0;tick<firstBlack;tick++)
   differences+=c.turn_luts[tick*1024]!=c.turn_luts[tick*1024+240*4];
#if RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES
  assert(differences==8);  // plain turns overlap too
#else
  assert(!differences);
  // q6 accepted order: both erase10..27, new black writes28..45.
  for(int tick=0;tick<48;tick++) {
   assert(c.turn_luts[tick*1024+240*4]==(tick>=10&&tick<=27?2:0));
   assert(c.turn_luts[tick*1024+15*4]==(tick>=28&&tick<=45?1:0));
  }
#endif
  mismatch+=differences;
  ricky_native_end(&c);assert(!live);
 }
 printf("old-black prefix action mismatches=%d\n",mismatch);
#if !RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES
 previous_tests();
#endif
 return 0;
}
''')
 args=['clang','-std=c11','-D_POSIX_C_SOURCE=200112L','-DCONFIG_IDF_TARGET_ESP32S31=1',
       '-DRICKYOS_PLUS_READER_QUALITY=1','-DRICKYOS_PLUS_READER_EARLY_ERASE=1',
       '-DRICKYOS_PLUS_READER_FINE_RIPPLE=1','-DRICKYOS_PLUS_READER_RIPPLE_SPREAD=15',
       '-DRICKYOS_PLUS_READER_SWEEP_SPREAD=4','-O1','-g','-Wall','-Wextra','-Werror',
       '-fsanitize=address,undefined','-I',str(p),'-I',str(DISPLAY),str(p/'probe.c'),
       str(DISPLAY/'waveforms/my_waveform.c'),str(DISPLAY/'waveforms/my_waveform_trim.c'),'-o',str(p/'test')]
 for overlap in [8,0]:
  subprocess.run([*args,f'-DRICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES={overlap}'],check=True)
  output=subprocess.check_output([str(p/'test')],text=True);print(f'overlap={overlap}: {output}',end='')
  # 4 styles (plain turns included) x 4 orientations x 8 overlapped phases.
  assert f'old-black prefix action mismatches={128 if overlap else 0}\n' in output
print('q10 order regression reproduced; q11 preserves q6 old-black erase order across all3 animations/4 orientations, compact spatial geometry and full native feed/OOM/UI/paper checks (ASan/UBSan). Optical acceptance pending.')
