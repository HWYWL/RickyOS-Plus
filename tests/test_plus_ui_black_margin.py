"""Compare every actual waveform cell with UI black restoration OFF and ON."""
from pathlib import Path
import struct
import subprocess
import tempfile

DISPLAY = Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2/components/metalio_board/display'
PROGRAM = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "waveforms/my_waveform.h"
int main(void) {
    my_waveform_init();
    unsigned char saved[7][48*16*4];
    assert(MY_WAVEFORM.num_modes==7);
    for(int m=0;m<7;++m) {
        const EpdWaveformMode* mode=MY_WAVEFORM.mode_data[m];
        const EpdWaveformPhases* p=mode->range_data[0];
        int header[2]={mode->type,p->phases};
        assert(p->phases<=48);
        memcpy(saved[m],p->luts,(size_t)p->phases*64);
        assert(fwrite(header,sizeof(header),1,stdout)==1);
        assert(fwrite(p->luts,64,p->phases,stdout)==(size_t)p->phases);
    }
    my_waveform_init();
    for(int m=0;m<7;++m) {
        const EpdWaveformPhases* p=MY_WAVEFORM.mode_data[m]->range_data[0];
        assert(memcmp(saved[m],p->luts,(size_t)p->phases*64)==0);
    }
}
'''

def unpack(data):
    result = {}
    cursor = 0
    while cursor < len(data):
        mode, phases = struct.unpack_from('=ii', data, cursor)
        cursor += 8
        result[mode] = (phases, data[cursor:cursor + phases * 64])
        cursor += phases * 64
    assert cursor == len(data) and len(result) == 7
    return result

def action(table, tick, target, old):
    return (table[(tick * 16 + target) * 4 + old // 4] >> (6 - 2 * (old % 4))) & 3

with tempfile.TemporaryDirectory(prefix='plus-ui-black-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(PROGRAM)
    (work / 'esp_log.h').write_text('#define ESP_LOGD(...) ((void)0)\n')
    waves = []
    for restored in (0, 2):
        binary = work / str(restored)
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', f'-DRICKYOS_PLUS_UI_BLACK_RESTORE_PHASES={restored}',
                        '-I', str(work), '-I', str(DISPLAY), str(work / 'test.c'),
                        str(DISPLAY / 'waveforms/my_waveform.c'),
                        str(DISPLAY / 'waveforms/my_waveform_trim.c'), '-o', str(binary)], check=True)
        waves.append(unpack(subprocess.run([str(binary)], stdout=subprocess.PIPE, check=True).stdout))
    before, after = waves
    assert before.keys() == after.keys()
    for mode in before:
        if mode != 0xB:
            assert before[mode] == after[mode], 'A reader/image/cleanup waveform changed'
    phases, old = before[0xB]
    new_phases, new = after[0xB]
    assert phases == new_phases == 37
    for target in range(16):
        for source in range(16):
            sequence = [action(old, f, target, source) for f in range(phases)]
            expected = sequence[:]
            if target == 0 and source != 0:
                last = max(f for f, value in enumerate(sequence) if value)
                assert last + 2 < phases - 3
                expected[last + 1:last + 3] = [1, 1]
            assert [action(new, f, target, source) for f in range(phases)] == expected
print('UI: exactly 2 original-margin black pulses in 15 changing-to-black cells; gray/white/held cells and all 6 other modes unchanged; idempotence and ASan/UBSan passed')
