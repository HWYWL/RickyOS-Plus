"""Production defaults and stale-cache rejection; no claim of radio acceptance."""
import importlib.util
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
path = ROOT / 'scripts/check_plus_web_memory.py'
spec = importlib.util.spec_from_file_location('plus_web_memory', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
defaults = (ROOT / 'firmware/rickyos-plus-1.2/sdkconfig.defaults').read_text()
for key, value in module.expected.items():
    assert f'{key}={"y" if value == "1" else value}' in defaults
with tempfile.TemporaryDirectory() as temporary:
    header = Path(temporary) / 'sdkconfig.h'
    header.write_text('\n'.join(f'#define {key} {value}' for key, value in module.expected.items()))
    module.check(header)
    for key in module.expected:
        header.write_text('\n'.join(f'#define {other} {value}' for other, value in module.expected.items() if other != key))
        try:
            module.check(header)
        except SystemExit as error:
            assert key in str(error)
        else:
            raise AssertionError('Stale configuration passed: ' + key)
    header.write_text('\n'.join(f'#define {key} {16384 if key == "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL" else value}'
                                for key, value in module.expected.items()))
    try:
        module.check(header)
    except SystemExit as error:
        assert 'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL' in str(error)
    else:
        raise AssertionError('Legacy 16 KiB internal preference passed')
print('Plus web memory: PSRAM preference, bounded DMA TX pool and stale configured-build rejection passed')
