"""Exercise direct byte-action hot ring: packed decoding failed hardware deadlines."""
from pathlib import Path
import subprocess
import tempfile
PORT=Path(__file__).resolve().parents[1]
DISPLAY=PORT/'firmware/rickyos-plus-1.2/components/metalio_board/display'
with tempfile.TemporaryDirectory(prefix='plus-fine-ripple-') as directory:
    p=Path(directory)
    (p/'esp_log.h').write_text('#define ESP_LOGD(...) ((void)0)\n')
    # Include the actual existing scheduler tests, then inspect the ring on a
    # full-sized delay field and verify every action on every ring wrap.
    (p/'test.c').write_text('#define main previous_tests\n#include "'+str(PORT/'tests/native_turn_host.c')+'"\n#undef main\n'+r"""
int main(void) {
 previous_tests();
 enum{S=RICKYOS_PLUS_READER_RIPPLE_SPREAD};
 for(int axis=0;axis<=1024;axis++) {
  assert(ricky_ripple_delay(axis,S)<=S);
  if(axis)assert(ricky_ripple_delay(axis,S)>=ricky_ripple_delay(axis-1,S));
 }
 assert(ricky_ripple_delay(0,S)==0 && ricky_ripple_delay(1024,S)==S);
 RenderContext_t c=context(1216,684);
 EpdWaveformPhases wave={.phases=48};allocations=fail_at=0;
 ricky_native_begin(&c,&wave,(RickyNativeTurn){.style=3,.orientation=0,
    .has_origin=true,.origin_x=512,.origin_y=512});
 assert(c.cycle_frames==48+S && sizeof(s_native_turn_window)==6144);
 unsigned seen=0;
 for(int i=0;i<c.turn_cols*((684+15)/16);i++)seen|=1u<<c.turn_delays[i];
 assert((seen&((1u<<S)-1))==((1u<<S)-1)); // All inner launch positions, not eight broad bands.
 memset(s_native_turn_window,0xa5,sizeof(s_native_turn_window));
 uint8_t expectedRing[6144];memset(expectedRing,0xa5,sizeof(expectedRing));
 for(int tick=0;tick<48+S;tick++) {
  c.turn_producer_tick=tick;ricky_turn_prepare_window(&c);
  for(int phase=tick>S?tick-S:0;phase<=tick&&phase<48;phase++)
   for(int pair=0;pair<256;pair++)expectedRing[(phase%(S+1))*256+pair]=c.turn_luts[phase*1024+pair*4];
  assert(memcmp(expectedRing,s_native_turn_window,6144)==0);
  for(int delay=0;delay<=S;delay++) {
   int phase=ricky_turn_phase(tick,delay,48);if(phase<0)continue;
   const uint8_t* lut=ricky_turn_lut(&c,phase);
   uint32_t input[64];uint8_t output[64];
   for(int pair=0;pair<256;pair++)((uint8_t*)input)[pair]=pair;
   ricky_turn_lookup(&c,input,output,lut,256);
   for(int pair=0;pair<256;pair++) {
    unsigned expected=c.turn_luts[phase*1024+pair*4];
    assert(((output[pair/4]>>(6-2*(pair%4)))&3)==expected);
   }
   for(int word=0;word<64;word++)assert(ricky_turn_word(&c,input+word,lut)==output[word]);
  }

 }
 // A sequential step must not reread still-live PSRAM source phases.
 c.turn_producer_tick=S;ricky_turn_prepare_window(&c); // rewind: full seed
 uint8_t saved[256];for(int pair=0;pair<256;pair++) {
  saved[pair]=c.turn_luts[1*1024+pair*4];c.turn_luts[1*1024+pair*4]=0;
 }
 c.turn_producer_tick=S+1;ricky_turn_prepare_window(&c);
 assert(memcmp(ricky_turn_lut(&c,1),saved,256)==0);
 for(int pair=0;pair<256;pair++)c.turn_luts[1*1024+pair*4]=saved[pair];
 for(int tick=S+1;tick>=0;tick-=6) { // repeated tick and backwards jumps fully seed
  c.turn_producer_tick=tick;ricky_turn_prepare_window(&c);
  for(int phase=tick>S?tick-S:0;phase<=tick;phase++)for(int pair=0;pair<256;pair++)
   assert(ricky_turn_lut(&c,phase)[pair]==c.turn_luts[phase*1024+pair*4]);
 }
 ricky_native_end(&c);assert(!live);
 puts("Fine ripple: configurable launch positions, complete48 phases, all256 byte actions, repeated ring wrap, full6144B write set/incremental reuse/rewind recovery, UI/default/OOM/rotations (ASan/UBSan) PASS");
}
""")
    command=['clang','-std=c11','-D_POSIX_C_SOURCE=200112L','-DCONFIG_IDF_TARGET_ESP32S31=1',
             '-DRICKYOS_PLUS_READER_QUALITY=1','-DRICKYOS_PLUS_READER_EARLY_ERASE=1',
             '-DRICKYOS_PLUS_READER_FINE_RIPPLE=1','-O1','-g','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-I',str(p),'-I',str(DISPLAY),str(p/'test.c'),
             str(DISPLAY/'waveforms/my_waveform.c'),str(DISPLAY/'waveforms/my_waveform_trim.c'),'-o',str(p/'test')]
    for spread in [4,5,15,23]:
        subprocess.run([*command,f'-DRICKYOS_PLUS_READER_RIPPLE_SPREAD={spread}'],check=True)
        subprocess.run([str(p/'test')],check=True)
    for spread in [3,24]:
        result=subprocess.run([*command,f'-DRICKYOS_PLUS_READER_RIPPLE_SPREAD={spread}'],capture_output=True,text=True)
        assert result.returncode and 'Reader ripple spread must be 4..23' in result.stderr
