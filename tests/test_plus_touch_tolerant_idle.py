"""A bare touch lets the reader's next-page build finish; any other cancel stops it."""
from pathlib import Path
import re
import subprocess
import tempfile

APP = Path(__file__).resolve().parents[1] / 'firmware/rickyos-plus-1.2/upstream'
header = (APP / 'src/activities/ActivityManager.h').read_text()


def member(signature):
    start = header.index(signature)
    brace = header.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (header[end] == '{') - (header[end] == '}')
        end += 1
    return header[start:end]


fields = '\n'.join(line for line in header.splitlines()
                   if re.match(r'\s*std::atomic<(uint32_t|const char\*|bool)> (idleRenderGeneration|lastIdleCancel|'
                               r'lastIdleCancelRearms|idleHardCancels|touchTolerantIdle|touchTolerantHardBase)\b', line))
program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
struct ActivityManager {
''' + fields + r'''
 std::atomic<bool> requestedUpdate{false}; bool switching=false;
 bool isSwitchPending() const {return switching;}
''' + member('void cancelIdleRender(') + '\n' + member('bool idleRenderCancelled(') + '\n' + \
    member('void setTouchTolerantIdle(') + r'''
};
int main(){
 ActivityManager m; uint32_t g=m.idleRenderGeneration.load();
 assert(!m.idleRenderCancelled(g));
 m.cancelIdleRender("touch",true); assert(m.idleRenderCancelled(g));  // ordinary idle work still stops
 g=m.idleRenderGeneration.load(); m.setTouchTolerantIdle(true);
 m.cancelIdleRender("touch",true); assert(!m.idleRenderCancelled(g));  // next-page build survives a touch
 m.cancelIdleRender("input"); assert(m.idleRenderCancelled(g));        // but not a key or other cancel
 m.setTouchTolerantIdle(false); g=m.idleRenderGeneration.load(); m.setTouchTolerantIdle(true);
 m.requestedUpdate=true; assert(m.idleRenderCancelled(g)); m.requestedUpdate=false;
 m.switching=true; assert(m.idleRenderCancelled(g)); m.switching=false;
 m.setTouchTolerantIdle(false); m.cancelIdleRender("touch"); assert(m.idleRenderCancelled(g));
}
'''
with tempfile.TemporaryDirectory(prefix='touch-tolerant-') as directory:
    work = Path(directory)
    (work / 'check.cpp').write_text(program)
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(work / 'check.cpp'), '-o', str(work / 'check')], check=True)
    subprocess.run([str(work / 'check')], check=True)
print('Idle cancel: a bare touch spares the next-page build; keys, updates and switches still stop it')
