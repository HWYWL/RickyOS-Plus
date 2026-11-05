"""Compare actual production LUTs and their UI completion transform, OFF vs ON."""
from pathlib import Path
import ast
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DISPLAY = ROOT / 'firmware/rickyos-plus-1.2/components/metalio_board/display'
tree = ast.parse((ROOT / 'tests/test_plus_ui_black_margin.py').read_text())
program = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'PROGRAM' for t in n.targets))


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


with tempfile.TemporaryDirectory(prefix='plus-ui-white-') as folder:
    work = Path(folder)
    (work / 'test.c').write_text(program)
    (work / 'esp_log.h').write_text('#define ESP_LOGD(...) ((void)0)\n')
    result = []
    for enabled in (0, 1):
        binary = work / str(enabled)
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', f'-DRICKYOS_PLUS_UI_WHITE_MAINTENANCE={enabled}',
                        '-DRICKYOS_PLUS_READER_BLACK_RESTORE_PHASES=4',
                        '-I', str(work), '-I', str(DISPLAY), str(work / 'test.c'),
                        str(DISPLAY / 'waveforms/my_waveform.c'),
                        str(DISPLAY / 'waveforms/my_waveform_trim.c'), '-o', str(binary)], check=True)
        result.append(unpack(subprocess.check_output([str(binary)])))
    before, after = result
    for mode in before:
        assert before[mode][0] == after[mode][0]
        if mode != 0xB:
            assert before[mode] == after[mode], 'A reading/image/cleanup waveform changed'
    phases, old = before[0xB]
    new = after[0xB][1]
    assert phases == 37
    differences = []
    for target in range(16):
        for source in range(16):
            for tick in range(phases):
                a, b = action(old, tick, target, source), action(new, tick, target, source)
                if a != b:
                    differences.append((tick, target, source, a, b))
    assert len(differences) == 1
    tick, target, source, a, b = differences[0]
    assert target == source == 15 and a == 0 and b == 2
    assert tick <= phases - 4  # Keep the existing three final settling phases.
    # Use the real completion/prefix-removal code, not a parallel simulation.
    native = (DISPLAY / 'ricky_native_turn.inc').read_text()
    start = native.index('static void ricky_ui_lock_completion(')
    end = native.index('static void ricky_reader_lock_text_finish(', start)
    transform = native[start:end]
    (work / 'completion.cpp').write_text(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
''' + transform + r'''
int main(int argc,char** argv){
 assert(argc==3);unsigned char raw[37*64],bank[37*1024]{};
 FILE* file=fopen(argv[1],"rb");assert(file&&fread(raw,1,sizeof(raw),file)==sizeof(raw));fclose(file);
 for(int f=0;f<37;++f)for(int to=0;to<16;++to)for(int from=0;from<16;++from){
  uint32_t action=(raw[(f*16+to)*4+from/4]>>(6-2*(from%4)))&3;
  memcpy(bank+f*1024+(to*16+from)*4,&action,sizeof(action));
 }
 ricky_ui_lock_completion(bank,37);
 const int phases=ricky_ui_trim_idle_head(bank,37);
 int white=0;for(int f=0;f<phases;++f)white+=bank[f*1024+255*4]==2;
 assert(white==atoi(argv[2]));
 printf("%d\n",phases);
}
''')
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(work / 'completion.cpp'),
                    '-o', str(work / 'completion')], check=True)
    transformed_phases = []
    for i, data in enumerate((old, new)):
        path = work / f'ui-{i}.bin'
        path.write_bytes(data)
        transformed_phases.append(subprocess.check_output([str(work / 'completion'), str(path), str(i)]))
    assert transformed_phases[0] == transformed_phases[1], 'Animation gained scan ticks'
    # Exercise the production S31 native scheduler/feed adapter with this bank
    # enabled too, including all orientations, cleanup, OOM and HOLD handling.
    subprocess.run(['clang', '-std=c11', '-D_POSIX_C_SOURCE=200112L',
                    '-DCONFIG_IDF_TARGET_ESP32S31=1', '-DRICKYOS_PLUS_UI_WHITE_MAINTENANCE=1',
                    '-DRICKYOS_PLUS_READER_BLACK_RESTORE_PHASES=4',
                    '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(work), '-I', str(DISPLAY), str(ROOT / 'tests/native_turn_host.c'),
                    str(DISPLAY / 'waveforms/my_waveform.c'),
                    str(DISPLAY / 'waveforms/my_waveform_trim.c'),
                    '-o', str(work / 'native')], check=True)
    subprocess.run([str(work / 'native')], check=True)
print('UI-only white experiment: exactly one existing-budget maintenance pulse; all six other banks, '
      '255 other cells, phase count, completion timing and idempotence preserved (ASan/UBSan)')
