"""The Plus side page keys swap in clockwise landscape, not counter-clockwise (panel mounted reversed)."""
from pathlib import Path
import subprocess
import tempfile

APP = Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2/upstream'
source = (APP / 'src/MappedInputManager.cpp').read_text()
start = source.index('bool MappedInputManager::isNavDirectionSwapped')
end = source.index('\n}\n', start) + 3
program = r'''
#include <cassert>
struct GfxRenderer {enum Orientation {Portrait,LandscapeClockwise,PortraitInverted,LandscapeCounterClockwise};
 Orientation o=Portrait; Orientation getOrientation()const{return o;}};
struct {bool touch=true; bool hasTouch()const{return touch;}} gpio;
struct {bool frontButtonFollowOrientation=false;} SETTINGS;
struct MappedInputManager {GfxRenderer& renderer; bool isNavDirectionSwapped() const;};
''' + source[start:end] + r'''
int main(){
 GfxRenderer r; MappedInputManager m{r};
 const bool plus = FREEINK_DEVICE_METALIO_PLUS;
 r.o=GfxRenderer::Portrait; assert(!m.isNavDirectionSwapped());
 r.o=GfxRenderer::PortraitInverted; assert(m.isNavDirectionSwapped());
 r.o=GfxRenderer::LandscapeClockwise; assert(m.isNavDirectionSwapped()==plus);
 r.o=GfxRenderer::LandscapeCounterClockwise; assert(m.isNavDirectionSwapped()==!plus);
 gpio.touch=false; assert(!m.isNavDirectionSwapped());  // button-only boards keep the user toggle
}
'''
with tempfile.TemporaryDirectory(prefix='plus-landscape-keys-') as directory:
    for plus in (1, 0):
        work = Path(directory) / str(plus)
        work.mkdir()
        (work / 'check.cpp').write_text(program)
        subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', f'-DFREEINK_DEVICE_METALIO_PLUS={plus}',
                        '-fsanitize=address,undefined', str(work / 'check.cpp'), '-o', str(work / 'check')], check=True)
        subprocess.run([str(work / 'check')], check=True)
print('Side page keys: Plus swaps in clockwise landscape, other boards in counter-clockwise; portrait unchanged')
