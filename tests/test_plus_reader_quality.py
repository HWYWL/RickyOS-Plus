"""Compare opt-in complete reading wave with source GL16 and actual native feed."""
from pathlib import Path
import ast
import re
import struct
import subprocess
import tempfile

PORT=Path(__file__).resolve().parents[1]
DISPLAY=PORT/'firmware/rickyos-plus-1.2/components/metalio_board/display'
tree=ast.parse((PORT/'tests/test_plus_reader_ink.py').read_text())
program=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign)
             and any(isinstance(t,ast.Name) and t.id=='PROGRAM' for t in n.targets))


def unpack(data):
    tables,cursor={},0
    while cursor<len(data):
        mode,phases=struct.unpack_from('=ii',data,cursor);cursor+=8
        tables[mode]=(phases,data[cursor:cursor+phases*64]);cursor+=phases*64
    assert cursor==len(data) and len(tables)==7
    return tables


source=(DISPLAY/'waveforms/my_waveform_gl.h').read_text()
source=source[source.index('= {')+3:source.index('};')]
original=bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',source)).ljust(48*64,b'\0')
with tempfile.TemporaryDirectory(prefix='plus-reader-quality-') as directory:
    p=Path(directory);(p/'test.c').write_text(program)
    (p/'esp_log.h').write_text('#define ESP_LOGD(...) ((void)0)\n')
    common=['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
            '-I',str(p),'-I',str(DISPLAY),'-DRICKYOS_PLUS_UI_WHITE_MAINTENANCE=1',
            '-DRICKYOS_PLUS_READER_BLACK_RESTORE_PHASES=4','-DRICKYOS_PLUS_READER_RETAINED_BLACK_PULSE=17']
    sources=[str(DISPLAY/'waveforms/my_waveform.c'),str(DISPLAY/'waveforms/my_waveform_trim.c')]
    variants={}
    for quality in (None,0,1):
        flags=[] if quality is None else [f'-DRICKYOS_PLUS_READER_QUALITY={quality}']
        binary=p/str(quality)
        subprocess.run([*common,*flags,str(p/'test.c'),*sources,'-o',str(binary)],check=True)
        variants[quality]=unpack(subprocess.check_output([str(binary)]))
    assert variants[None]==variants[0]
    for mode,(phases,raw) in variants[1].items():
        if mode in (3,6,12):
            assert phases==48 and raw==original, 'Complete source wave shortened or changed'
        else:
            assert (phases,raw)==variants[0][mode], 'UI/image/full-clean waveform changed'
    assert all((original[(f*16+15)*4+3]&3)==0 for f in range(48)), 'Chrome sentinel must HOLD'
    for flags,marker in [(['-DRICKYOS_PLUS_READER_QUALITY=2'],'must be 0 or 1'),
                         (['-DRICKYOS_PLUS_READER_QUALITY=1','-DRICKYOS_PLUS_READER_SHORT_WAVEFORM=1'],'requires the full')]:
        r=subprocess.run([*common,*flags,'-c',sources[0],'-o',str(p/'invalid.o')],capture_output=True,text=True)
        assert r.returncode and marker in r.stderr
    subprocess.run([*common,'-D_POSIX_C_SOURCE=200112L','-DCONFIG_IDF_TARGET_ESP32S31=1',
                    '-DRICKYOS_PLUS_READER_QUALITY=1','-O1','-g',str(PORT/'tests/native_turn_host.c'),
                    *sources,'-o',str(p/'native')],check=True)
    subprocess.run([str(p/'native')],check=True)
print('Reader quality: actual source GL16 x all256 transitions,48 phases,default OFF,UI/cleanup unchanged,chrome HOLD,real native feed/AA/paper/motion/OOM passed (ASan/UBSan); optical A/B pending')
