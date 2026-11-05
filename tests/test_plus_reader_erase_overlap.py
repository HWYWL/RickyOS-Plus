"""Run real text timing for all three motion styles, not a waveform mock."""
from pathlib import Path
import re
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
DISPLAY = PORT / 'firmware/rickyos-plus-1.2/components/metalio_board/display'
source = (DISPLAY / 'ricky_native_turn.inc').read_text()


def actual_function(name):
    start = source.index('static void ' + name + '(')
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


text = (DISPLAY / 'waveforms/my_waveform_gl.h').read_text()
text = text[text.index('= {') + 3:text.index('};')]
raw = bytes(int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', text)).ljust(48 * 64, b'\0')

with tempfile.TemporaryDirectory(prefix='plus-erase-overlap-') as directory:
    p = Path(directory)
    (p / 'raw.bin').write_bytes(raw)
    (p / 'esp_log.h').write_text('#define ESP_LOGD(...) ((void)0)\n')
    (p / 'test.c').write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
''' + actual_function('ricky_reader_lock_text_finish') + '\n' +
        actual_function('ricky_reader_overlap_erase') + r'''
int main(int argc,char** argv) {
 assert(argc==2);
 unsigned char raw[48*64],bank[48*1024]={0},saved[48*1024];
 FILE* f=fopen(argv[1],"rb");assert(f&&fread(raw,1,sizeof(raw),f)==sizeof(raw));fclose(f);
 for(int tick=0;tick<48;tick++)for(int pair=0;pair<256;pair++) {
  int to=pair>>4,old=pair&15;
  bank[tick*1024+pair*4]=(raw[(tick*16+to)*4+old/4]>>(6-2*(old%4)))&3;
 }
 ricky_reader_lock_text_finish(bank,48);
 ricky_reader_overlap_erase(bank,48,RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES);memcpy(saved,bank,sizeof(bank));
 ricky_reader_overlap_erase(bank,48,RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES);assert(memcmp(saved,bank,sizeof(bank))==0);
 assert(fwrite(bank,1,sizeof(bank),stdout)==sizeof(bank));
}
''')
    common = ['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
              '-fsanitize=address,undefined', '-DRICKYOS_PLUS_READER_EARLY_ERASE=1']
    variants = {}
    for overlap in (0, 4, 8):
        subprocess.run([*common, f'-DRICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES={overlap}',
                        str(p / 'test.c'), '-o', str(p / 'test')], check=True)
        variants[overlap] = subprocess.check_output([str(p / 'test'), str(p / 'raw.bin')])
    for overlap in (4, 8):
        changed = 0
        for pair in range(256):
            before = [variants[0][tick * 1024 + pair * 4] for tick in range(48)]
            after = [variants[overlap][tick * 1024 + pair * 4] for tick in range(48)]
            if pair >> 4 != 15 or pair & 15 == 15:
                assert after == before, 'Black/gray/retained white changed'
                continue
            delay = max(0, 27 + overlap - max(i for i, v in enumerate(before) if v))
            assert after == ([0] * delay + before[:48 - delay]), 'Internal HOLD/pulse shifted incorrectly'
            assert [v for v in before if v] == [v for v in after if v], 'Active pulse removed/reordered'
            assert not any(after[-2:]), 'Settling tail lost'
            changed += after != before
        assert changed == 15
        # Check complete padded bank, not only the action byte.
        assert all(variants[overlap][n] == 0 for n in range(48 * 1024) if n % 4)
        erase = [variants[overlap][tick * 1024 + 240 * 4] for tick in range(48)]
        write = [variants[overlap][tick * 1024 + 15 * 4] for tick in range(48)]
        assert erase == [0] * (10 + overlap) + [2] * 18 + [0] * (20 - overlap)
        assert write == [0] * 28 + [1] * 18 + [0] * 2
        assert sum(a == 2 and b == 1 for a, b in zip(erase, write)) == overlap

    native = [*common, '-D_POSIX_C_SOURCE=200112L', '-DCONFIG_IDF_TARGET_ESP32S31=1',
              '-DRICKYOS_PLUS_READER_QUALITY=1', '-DRICKYOS_PLUS_READER_FINE_RIPPLE=1',
              '-DRICKYOS_PLUS_READER_RIPPLE_SPREAD=15',
              '-DRICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES=8',
              '-I', str(p), '-I', str(DISPLAY), str(PORT / 'tests/native_turn_host.c'),
              str(DISPLAY / 'waveforms/my_waveform.c'),
              str(DISPLAY / 'waveforms/my_waveform_trim.c'), '-o', str(p / 'native')]
    for sweep in (2, 3, 4, 7):
        subprocess.run([*native, f'-DRICKYOS_PLUS_READER_SWEEP_SPREAD={sweep}'], check=True)
        subprocess.run([str(p / 'native')], check=True)
    # Ripple's own, longer overlap beside the plain turns' (the release pairs 6 with 12),
    # and up to its 16-phase limit.
    for erase, ripple in ((6, 12), (0, 16)):
        args = [arg for arg in native if not arg.startswith('-DRICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES=')]
        subprocess.run([*args, f'-DRICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES={erase}',
                        f'-DRICKYOS_PLUS_READER_RIPPLE_OVERLAP_PHASES={ripple}',
                        '-DRICKYOS_PLUS_READER_SWEEP_SPREAD=3'], check=True)
        subprocess.run([str(p / 'native')], check=True)
    for flag, message in [('RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES=9', 'must be 0..8'),
                          ('RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES=-1', 'must be 0..8'),
                          ('RICKYOS_PLUS_READER_SWEEP_SPREAD=1', 'must be 2..7'),
                          ('RICKYOS_PLUS_READER_SWEEP_SPREAD=8', 'must be 2..7'),
                          ('RICKYOS_PLUS_READER_RIPPLE_OVERLAP_PHASES=17', 'must be 0..16'),
                          ('RICKYOS_PLUS_READER_EARLY_ERASE=0', 'requires early erase')]:
        name = flag.split('=')[0]
        rejected = [arg for arg in native if not arg.startswith('-D' + name + '=')]
        result = subprocess.run([*rejected, '-D' + flag], capture_output=True, text=True)
        assert result.returncode and message in result.stderr

print('All reader motion erase overlap: 15 white-target transitions delayed, all pulses/internal HOLD preserved; '
      'black/gray/chrome unchanged; actual native skim/arc/ripple, sweep3/4/7, OFF/UI/paper/OOM/rotations passed '
      '(ASan/UBSan). Electrical timing and optical ghosting still require hardware comparison.')
