"""Bound the retained-black maximum using real tables and real native feed code."""
from pathlib import Path
import ast
import re
import struct
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
DISPLAY = PORT/'firmware/rickyos-plus-1.2/components/metalio_board/display'
tree = ast.parse((PORT/'tests/test_plus_reader_ink.py').read_text())
program = next(ast.literal_eval(node.value) for node in tree.body if isinstance(node, ast.Assign)
               and any(isinstance(target, ast.Name) and target.id == 'PROGRAM' for target in node.targets))


def unpack(raw):
    result, cursor = {}, 0
    while cursor < len(raw):
        mode, phases = struct.unpack_from('=ii', raw, cursor)
        cursor += 8
        result[mode] = phases, raw[cursor:cursor+phases*64]
        cursor += phases*64
    assert cursor == len(raw) and len(result) == 7
    return result


def sequence(data, phases, target, old):
    return [(data[(tick*16+target)*4+old//4] >> (6-2*(old % 4))) & 3 for tick in range(phases)]


def full_source(name):
    text = (DISPLAY/f'waveforms/my_waveform_{name}.h').read_text()
    text = text[text.index('= {')+3:text.index('};')]
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', text)).ljust(48*64, b'\0')


with tempfile.TemporaryDirectory(prefix='plus-retained-ink-') as directory:
    work = Path(directory)
    (work/'test.c').write_text(program)
    (work/'esp_log.h').write_text('#define ESP_LOGD(...) ((void)0)\n')
    flags = ['-DRICKYOS_PLUS_READER_BLACK_RESTORE_PHASES=4', '-DRICKYOS_PLUS_UI_WHITE_MAINTENANCE=1']
    sources = [str(DISPLAY/'waveforms/my_waveform.c'), str(DISPLAY/'waveforms/my_waveform_trim.c')]
    command = ['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-I', str(work), '-I', str(DISPLAY), *flags]
    variants = {}
    for pulse in [None, *range(5, 18)]:
        binary = work/str(pulse)
        defines = [] if pulse is None else [f'-DRICKYOS_PLUS_READER_RETAINED_BLACK_PULSE={pulse}']
        subprocess.run([*command, *defines, str(work/'test.c'), *sources, '-o', str(binary)], check=True)
        variants[pulse] = unpack(subprocess.check_output([str(binary)]))
    assert variants[None] == variants[5], 'Default must retain the preceding ui3 drive'
    baseline = variants[5]
    gc, gl = full_source('gc'), full_source('gl')
    for pulse in range(5, 18):
        for mode, (phases, raw) in variants[pulse].items():
            before_phases, before = baseline[mode]
            assert phases == before_phases
            if mode not in (3, 12):
                assert raw == before, 'UI, images or full cleanup changed'
                continue
            assert phases == 37
            for target in range(16):
                for old in range(16):
                    after_seq = sequence(raw, phases, target, old)
                    if target != 0 or old != 0:
                        assert after_seq == sequence(before, phases, target, old), 'Another transition changed'
                    else:
                        assert after_seq == [2]*pulse+[1]*pulse+[0]*(phases-2*pulse)
                        for original in (gc, gl):
                            full = sequence(original, 48, 0, 0)
                            assert after_seq.count(1) <= full.count(1)
                            assert after_seq.count(2) <= full.count(2)
                        assert after_seq[-3:] == [0]*3, 'Settling tail consumed'
    # Reject both smaller and larger invalid values at compile time.
    for invalid in (0, 4, 18, 64):
        result = subprocess.run([*command, f'-DRICKYOS_PLUS_READER_RETAINED_BLACK_PULSE={invalid}',
                                 '-c', sources[0], '-o', str(work/'invalid.o')], capture_output=True, text=True)
        assert result.returncode and 'Retained black re-drive' in result.stderr
    # Execute real S31 native scheduling and line output, including AA OFF/ON,
    # all spatial styles/orientations, OOM fallback and paper gray handling.
    subprocess.run([*command, '-D_POSIX_C_SOURCE=200112L', '-DCONFIG_IDF_TARGET_ESP32S31=1',
                    '-DRICKYOS_PLUS_READER_RETAINED_BLACK_PULSE=17', '-O1', '-g',
                    str(PORT/'tests/native_turn_host.c'), *sources, '-o', str(work/'native')], check=True)
    subprocess.run([str(work/'native')], check=True)
print('Retained black max: 17 erase + 17 dark in 37 phases, three settling HOLDs, bounded by full GC/GL; all other cells/banks unchanged, defaults preserved, real native feed and OOM passed (ASan/UBSan); optical acceptance pending')
