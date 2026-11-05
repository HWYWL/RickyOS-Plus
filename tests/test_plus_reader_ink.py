"""Execute actual waveform assembly: reader black drive bounded by full GC/GL.

This proves electrical instruction changes, not measured ink density. Compare
the same page on hardware before accepting this optical experiment.
"""
from pathlib import Path
import hashlib
import re
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
    unsigned char saved[7][48*16*4];
    my_waveform_init();
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
    tables, cursor = {}, 0
    while cursor < len(data):
        mode, phases = struct.unpack_from('=ii', data, cursor)
        cursor += 8
        tables[mode] = phases, data[cursor:cursor + phases*64]
        cursor += phases*64
    assert cursor == len(data) and len(tables) == 7
    return tables


def sequence(table, phases, target, old):
    return [(table[(f*16+target)*4+old//4] >> (6-2*(old % 4))) & 3 for f in range(phases)]


def source_data(name):
    source = (DISPLAY / f'waveforms/my_waveform_{name}.h').read_text()
    source = source[source.index('= {')+3:source.index('};')]
    raw = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', source))
    # GC declares 48 frames but explicitly initializes 47; C zero-fills the tail.
    assert len(raw) <= 48*64 and len(raw) % 64 == 0
    return raw.ljust(48*64, b'\0')


full_gl, full_gc = source_data('gl'), source_data('gc')
with tempfile.TemporaryDirectory(prefix='plus-reader-ink-') as folder:
    temp = Path(folder)
    (temp/'test.c').write_text(PROGRAM)
    (temp/'esp_log.h').write_text('#define ESP_LOGD(...) ((void)0)\n')
    variants = {}
    for limit in (0, 1, 2, 3, 4, None, 5):
        binary = temp/str(limit)
        defines = [] if limit is None else [f'-DRICKYOS_PLUS_READER_BLACK_RESTORE_PHASES={limit}']
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-DRICKYOS_PLUS_UI_WHITE_MAINTENANCE=1',
                        *defines, '-I', str(temp), '-I', str(DISPLAY),
                        str(temp/'test.c'), str(DISPLAY/'waveforms/my_waveform.c'),
                        str(DISPLAY/'waveforms/my_waveform_trim.c'), '-o', str(binary)], check=True)
        # Default OFF is checked against the released hashes below as well.
        variants[0 if limit is None else limit] = unpack(subprocess.check_output([str(binary)]))
    baseline = variants[0]
    # The OFF switch must reproduce the actual 1.2.8 production reading tables.
    for mode, digest in [(3, 'a578a3158460251ade177b9e3a1eb716f154dd9d5bbf16bce05da55dfad1e676'),
                         (12, '4b8b140899fb8da0372463cfcc2c3f2ad9e4be1976610598ae01327815cbac8d')]:
        assert hashlib.sha256(baseline[mode][1]).hexdigest() == digest
    for limit, tables in variants.items():
        for mode, (phases, raw) in tables.items():
            old_phases, old_raw = baseline[mode]
            assert phases == old_phases
            if mode not in (3, 12):
                assert raw == old_raw, 'UI/image/cleanup waveform changed'
                continue
            assert phases == 37
            changed = 0
            for target in range(16):
                for old in range(16):
                    before = sequence(old_raw, phases, target, old)
                    after = sequence(raw, phases, target, old)
                    if target != 0 or old == 0:
                        assert before == after, 'Gray edge, erase or retained black changed'
                        continue
                    cap = min(sequence(full_gl, 48, 0, old).count(1),
                              sequence(full_gc, 48, 0, old).count(1))
                    assert after.count(1) == min(before.count(1)+limit, cap)
                    assert after.count(2) == before.count(2)
                    assert after.count(3) == before.count(3)
                    # Existing active instructions are preserved, additions only
                    # consume trailing HOLD and retain >=3 settling phases.
                    last_before = max(i for i, action in enumerate(before) if action)
                    assert after[:last_before+1] == before[:last_before+1]
                    for tick, (a, b) in enumerate(zip(before, after)):
                        if a != b:
                            assert a == 0 and b == 1 and last_before < tick < phases-3
                            changed += 1
                    if limit >= 4:
                        assert after.count(1) == cap
            assert changed == 15*min(limit, 4), (mode, limit, changed)
            assert sequence(raw, phases, 0, 0) == [2]*5+[1]*5+[0]*27
            assert sequence(raw, phases, 0, 15).count(1) == 13+min(limit, 4)
print('Reader ink: opt-in 17 vs released/default 13 black pulses, full-GC/GL bounds, all 256 transitions, 37 scans, UI/image/gray/retained ink unchanged; ASan/UBSan and idempotence passed; optical A/B pending')
