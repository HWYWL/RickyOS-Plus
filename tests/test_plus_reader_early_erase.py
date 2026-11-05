"""Run actual full-quality text timing: clear old ink coherently without cutting drive."""
from pathlib import Path
import re
import subprocess
import tempfile

PORT=Path(__file__).resolve().parents[1]
DISPLAY=PORT/'firmware/rickyos-plus-1.2/components/metalio_board/display'
source=(DISPLAY/'ricky_native_turn.inc').read_text()
a=source.index('static void ricky_reader_lock_text_finish(');brace=source.index('{',a)
end=brace+1;depth=1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
actual=source[a:end]
text=(DISPLAY/'waveforms/my_waveform_gl.h').read_text();text=text[text.index('= {')+3:text.index('};')]
raw=bytes(int(v,16) for v in re.findall(r'0x([0-9a-fA-F]{2})',text)).ljust(48*64,b'\0')


def original(pair):
    to,old=pair>>4,pair&15
    return [(raw[(f*16+to)*4+old//4]>>(6-2*(old%4)))&3 for f in range(48)]


with tempfile.TemporaryDirectory(prefix='plus-early-erase-') as directory:
    p=Path(directory);(p/'raw.bin').write_bytes(raw)
    (p/'esp_log.h').write_text('#define ESP_LOGD(...) ((void)0)\n')
    (p/'test.c').write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
''' + actual + r'''
int main(int argc,char** argv){
 assert(argc==2);unsigned char raw[48*64],bank[48*1024]={0},saved[48*1024];
 FILE* f=fopen(argv[1],"rb");assert(f&&fread(raw,1,sizeof(raw),f)==sizeof(raw));fclose(f);
 for(int tick=0;tick<48;++tick)for(int pair=0;pair<256;++pair){
  int to=pair>>4,old=pair&15;
  bank[tick*1024+pair*4]=(raw[(tick*16+to)*4+old/4]>>(6-2*(old%4)))&3;
 }
 ricky_reader_lock_text_finish(bank,48);memcpy(saved,bank,sizeof(bank));
 ricky_reader_lock_text_finish(bank,48);assert(memcmp(saved,bank,sizeof(bank))==0);
 assert(fwrite(bank,1,sizeof(bank),stdout)==sizeof(bank));
}
''')
    variants={}
    for enabled in (0,1):
        binary=p/str(enabled)
        subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                        f'-DRICKYOS_PLUS_READER_EARLY_ERASE={enabled}',str(p/'test.c'),'-o',str(binary)],check=True)
        variants[enabled]=subprocess.check_output([str(binary),str(p/'raw.bin')])
    changed=0
    for pair in range(256):
        before=original(pair);after=[variants[1][tick*1024+pair*4] for tick in range(48)]
        baseline=[variants[0][tick*1024+pair*4] for tick in range(48)]
        assert baseline==before
        if pair>>4!=15 or pair&15==15:
            assert after==before, 'Black/gray strokes or retained-white chrome changed'
            continue
        assert [v for v in before if v]==[v for v in after if v], 'Lost/reordered pulse'
        first=next(i for i,v in enumerate(before) if v);last=max(i for i,v in enumerate(before) if v)
        left=min(first,last-27)
        assert after==before[left:]+[0]*left, 'Internal HOLD or non-exterior timing changed'
        assert max(i for i,v in enumerate(after) if v)==max(27,last-first)
        changed+=after!=before
    assert changed==15
    pure_white=[variants[1][tick*1024+240*4] for tick in range(48)]
    assert pure_white==[0]*10+[2]*18+[0]*20 # Old black clears before new ink starts at28.
    common=['clang','-std=c11','-D_POSIX_C_SOURCE=200112L','-DCONFIG_IDF_TARGET_ESP32S31=1',
            '-DRICKYOS_PLUS_READER_QUALITY=1','-DRICKYOS_PLUS_READER_EARLY_ERASE=1',
            '-DRICKYOS_PLUS_UI_WHITE_MAINTENANCE=1','-DRICKYOS_PLUS_READER_BLACK_RESTORE_PHASES=4',
            '-DRICKYOS_PLUS_READER_RETAINED_BLACK_PULSE=17','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-I',str(p),'-I',str(DISPLAY)]
    subprocess.run([*common,str(PORT/'tests/native_turn_host.c'),str(DISPLAY/'waveforms/my_waveform.c'),
                    str(DISPLAY/'waveforms/my_waveform_trim.c'),'-o',str(p/'native')],check=True)
    subprocess.run([str(p/'native')],check=True)
    for flag, message in [('RICKYOS_PLUS_READER_EARLY_ERASE=2', 'must be 0 or 1'),
                          ('RICKYOS_PLUS_READER_QUALITY=0', 'requires the complete quality')]:
        name=flag.split('=')[0]
        rejected=[value for value in common if not value.startswith('-D'+name+'=')]
        result=subprocess.run([*rejected,'-D'+flag,str(PORT/'tests/native_turn_host.c'),
                               str(DISPLAY/'waveforms/my_waveform.c'),str(DISPLAY/'waveforms/my_waveform_trim.c'),
                               '-o',str(p/'invalid')],capture_output=True,text=True)
        assert result.returncode and message in result.stderr

print('Early text erase: old black clears at10..27 instead28..45; all18 write-black pulses and gray/retained cells exact,15 white transitions keep every pulse/internal HOLD,48/55 ticks/native/motion/paper/OOM passed (ASan/UBSan); optical acceptance pending')
