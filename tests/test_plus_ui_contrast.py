"""Plus menu tones deepen; Pico presets and black/white endpoints stay intact."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
app = root / 'firmware/rickyos-plus-1.2/upstream'
renderer = (app / 'lib/GfxRenderer/GfxRenderer.cpp').read_text()
tone_constants = renderer[renderer.index('#ifndef RICKYOS_GRAY_PRESET'):renderer.index('#include <memory>')]
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    for plus, preset, expected in ((1, 2, (2, 3, 5, 12, 4, 2)),
                                    (0, 2, (2, 5, 7, 12, 7, 4)),
                                    (0, 3, (3, 6, 8, 12, 8, 5)),
                                    (0, 0, (4, 8, 11, 13, 11, 7))):
        code = '#include <cassert>\n#include "components/RickyDesignTokens.h"\n' + tone_constants
        code += 'int main(){using namespace ricky::ds::gray;\n'
        for name, value in zip(('kInk2', 'kMute', 'kLine', 'kTint', 'kLightGrayLevel', 'kDarkGrayLevel'), expected):
            code += f'static_assert({name}=={value});\n'
        code += 'static_assert(kInk==0&&kPaper==15);static_assert(kInk<kInk2&&kInk2<kMute);}\n'
        (temp / 'test.cpp').write_text(code)
        subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        f'-DFREEINK_DEVICE_METALIO_PLUS={plus}', f'-DRICKYOS_GRAY_PRESET={preset}',
                        '-I' + str(app / 'src'), str(temp / 'test.cpp'), '-o', str(temp / 'test')], check=True)
        subprocess.run([str(temp / 'test')], check=True)
print('Actual Plus UI tokens/fill tones, legacy/Pico presets and black/white endpoints passed')
