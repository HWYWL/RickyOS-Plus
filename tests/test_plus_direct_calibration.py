"""Calibration is opt-in, known-white/one-direction only, and invalidates optical state."""
from pathlib import Path
import subprocess
import tempfile
from test_plus_regional_gray import PORT, shim, high, backend, driver, run_program

timed_stub = r'''
int timedCalls=0;
EpdDrawError epd_calibrate_timed_samples(const EpdRect* cells,int count){
 ++timedCalls;assert(count==16);
 for(int i=0;i<16;++i){
  assert(cells[i].x==W/2 && cells[i].width==W/6);
  assert(cells[i].y>=0 && cells[i].y+cells[i].height<=H);
  if(i)assert(cells[i].y<cells[i-1].y);
 }
 if(failDraw && --failDraw==0)return EPD_DRAW_EMPTY_LINE_QUEUE;
 return EPD_DRAW_SUCCESS;
}
'''
program = r'''
#define RICKYOS_PLUS_GRAY_CALIBRATION 1
#include "PlusGrayCalibration.h"
''' + shim + high + backend + timed_stub + driver + r'''
int sample(const uint8_t* p,int x,int y) {
 int nx=y,ny=H-1-x;
 return (p[ny*W/2+nx/2]>>((nx&1)*4))&15;
}
int main(){
 using namespace freeink;
 EpdBus bus;auto& d=metalioPlusDriver();d.begin(bus);
 uint8_t proxy[W*H/8];memset(proxy,255,sizeof(proxy));
 plus::GrayCalibrationCard card{W,H};
 d.display(bus,proxy,nullptr,RefreshMode::Fast,false);
 assert(cleared==1 && d.opticalState()==OpticalState::Bw);
 d.beginGrayscale16();metalioPlusSetGray16Profile(1);
 assert(d.commitGrayscale16(proxy));
 assert(cleared==2 && selectedModes.back()==MODE_GC16 && calls.empty() && timedCalls==1);
 assert(d.opticalState()==OpticalState::Unknown && powered==off);
 // Reference has one dash, samples two, readable even after 180-degree rotation.
 assert(sample(back,card.margin()+5,card.upper()-22)==0);
 assert(sample(back,card.margin()+33,card.upper()-22)==15);
 assert(sample(back,card.margin()+5,card.lower()-22)==0);
 assert(sample(back,card.margin()+33,card.lower()-22)==0);
 for(int i=0;i<16;++i) {
   int x=(card.boundary(i)+card.boundary(i+1))/2;
   assert(sample(back,x,card.upper()+card.bandHeight()/2)==i);
   // Hardware timing samples are NOT published as trusted optical gray codes.
   assert(sample(back,x,card.lower()+card.bandHeight()/2)==15);
 }
 // Idle/redraw/cycle callbacks cannot dismiss the card before photographing.
 int before=powered;
 for(int profile:{1,2,0,1,0}){
   d.beginGrayscale16();metalioPlusSetGray16Profile(profile);
   assert(d.commitGrayscale16(proxy));assert(powered==before);
   assert(d.opticalState()==OpticalState::Unknown);
 }
 // Home/menu navigation leaves it, cleaning rather than trusting sample codes.
 d.display(bus,proxy,nullptr,RefreshMode::Fast,false);
 assert(cleared==3 && d.opticalState()==OpticalState::Bw);
 d.beginGrayscale16();metalioPlusSetGray16Profile(1);
 assert(d.commitGrayscale16(proxy));assert(cleared==4);
 assert(d.opticalState()==OpticalState::Unknown && powered==off); // Re-enter without a reboot.
 // Explicit native UI navigation also exits and re-arms, unlike profile 0.
 d.beginGrayscale16();metalioPlusSetGray16Profile(4);
 assert(d.commitGrayscale16(proxy));assert(cleared==5);
 assert(d.opticalState()==OpticalState::Gray);
 d.beginGrayscale16();metalioPlusSetGray16Profile(1);
 assert(d.commitGrayscale16(proxy));assert(cleared==6);
 assert(d.opticalState()==OpticalState::Unknown && powered==off);
}
'''
run_program(program)
failure = program[:program.index('int main(){')] + r'''
int main(){
 using namespace freeink;
 EpdBus bus;auto& d=metalioPlusDriver();d.begin(bus);
 uint8_t proxy[W*H/8];memset(proxy,255,sizeof(proxy));
 failDraw=1;
 d.beginGrayscale16();metalioPlusSetGray16Profile(1);
 assert(!d.commitGrayscale16(proxy));
 assert(d.opticalState()==OpticalState::Unknown && powered==off);
 d.display(bus,proxy,nullptr,RefreshMode::Fast,false);
 assert(cleared==2 && d.opticalState()==OpticalState::Bw);
 d.beginGrayscale16();metalioPlusSetGray16Profile(1);
 assert(d.commitGrayscale16(proxy));
 assert(d.opticalState()==OpticalState::Unknown && powered==off);
}
'''
run_program(failure)

wave = r'''
#include <assert.h>
#include "waveforms/my_waveform.h"
int main(void){
 my_waveform_init();
 assert(MY_WAVEFORM.num_modes==7); // Six accepted modes plus the isolated reading-paper table.
 for(int m=0;m<MY_WAVEFORM.num_modes;++m)
   assert(MY_WAVEFORM.mode_data[m]->type!=10); // Obsolete whole-phase experiment removed.
}
'''
display = PORT/'components/metalio_board/display'
with tempfile.TemporaryDirectory(prefix='plus-direct-calibration-') as folder:
    p=Path(folder)
    (p/'test.c').write_text(wave)
    (p/'esp_log.h').write_text('#define ESP_LOGD(...) ((void)0)\n')
    subprocess.run([
        'clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-DRICKYOS_PLUS_GRAY_CALIBRATION=1','-I',str(p),'-I',str(display),
        str(p/'test.c'),str(display/'waveforms/my_waveform.c'),
        str(display/'waveforms/my_waveform_trim.c'),'-o',str(p/'test'),
    ],check=True)
    subprocess.run([str(p/'test')],check=True)
for component in ('rickyos_display','metalio_board'):
    cmake=(PORT/'components'/component/'CMakeLists.txt').read_text()
    assert 'if("$ENV{RICKYOS_PLUS_GRAY_CALIBRATION}" STREQUAL "1")' in cmake
print('Actual calibration driver: opt-in timed samples, real white baseline, optical invalidation, markers, repeat entry and failure/power cleanup passed (ASan/UBSan)')
