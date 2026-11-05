"""Read-only analysis of the production reader table; not optical calibration."""
from pathlib import Path
import subprocess
import tempfile

DISPLAY = Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2/components/metalio_board/display'
PROGRAM = r'''
#include <assert.h>
#include <stdio.h>
#include "waveforms/my_waveform.h"
static int action(const EpdWaveformPhases* p,int f,int to,int from) {
    return (p->luts[(f*16+to)*4+from/4]>>(6-2*(from%4)))&3;
}
int main(void) {
    my_waveform_init();
    const EpdWaveformPhases* p=NULL;
    for(int i=0;i<MY_WAVEFORM.num_modes;i++)
        if(MY_WAVEFORM.mode_data[i]->type==MY_MODE_READER_TURN)
            p=MY_WAVEFORM.mode_data[i]->range_data[0];
    assert(p && p->phases==37);
    puts("Production MY_MODE_READER_TURN (0=black,15=white); zero-based phases.");
    puts("Commands only: .=HOLD D=darken W=whiten. Not measured pixel luminance.");
    for(int from=15;from>=0;from-=15) {
        for(int to=0;to<16;to++) {
            int first=-1,last=-1,dark=0,white=0;
            char sequence[65]={0};
            for(int f=0;f<p->phases;f++) {
                int a=action(p,f,to,from);
                assert(a<3);
                sequence[f]=a==1?'D':a==2?'W':'.';
                if(a && first<0)first=f;
                if(a)last=f;
                dark+=a==1;white+=a==2;
            }
            printf("%2d -> %2d first=%2d last=%2d dark=%2d white=%2d %s\n",
                   from,to,first,last,dark,white,sequence);
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix='plus-reader-wave-report-') as folder:
    work = Path(folder)
    (work / 'esp_log.h').write_text('#define ESP_LOGD(...) ((void)0)\n')
    (work / 'report.c').write_text(PROGRAM)
    binary = work / 'report'
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(work), '-I', str(DISPLAY),
                    str(work / 'report.c'), str(DISPLAY / 'waveforms/my_waveform.c'),
                    str(DISPLAY / 'waveforms/my_waveform_trim.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
