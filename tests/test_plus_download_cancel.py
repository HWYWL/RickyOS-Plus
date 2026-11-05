"""Exercise the actual HTTP abort callback with no incoming body chunks."""
from pathlib import Path
import subprocess, tempfile
source = Path('firmware/rickyos-plus-1.2/upstream/src/network/HttpDownloader.cpp').read_text()
start = source.index('[&rejectedRedirect, cancelFlag, &pollInput, &lastInputPoll, started, totalBudgetMs]')
end = source.index('\n      });', start)
callback = source[start:end] + '\n}'
program = r'''
#include <cassert>
#include <cstdint>
#include <functional>
uint32_t tick;
uint32_t millis() { return tick; }
int main() {
 const uint32_t started=tick, totalBudgetMs=tick;
 bool rejectedRedirect=false, cancelled=false;
 const bool* cancelFlag=&cancelled;
 uint32_t lastInputPoll=uint32_t(0)-20;
 int polls=0;
 std::function<void()> pollInput=[&]{ if (++polls==3) cancelled=true; };
 auto abortCheck=CALLBACK;
 assert(!abortCheck() && polls==1);
 for (tick=1; tick<20; ++tick) assert(!abortCheck());
 assert(polls==1);
 assert(!abortCheck() && polls==2);
 tick=40; assert(abortCheck() && polls==3);
 cancelled=false; pollInput=nullptr; tick=80; assert(!abortCheck());
 rejectedRedirect=true; assert(abortCheck());
 rejectedRedirect=false; cancelFlag=nullptr; assert(!abortCheck());
 lastInputPoll=UINT32_MAX-10; tick=10;
 pollInput=[&]{cancelled=true;}; cancelFlag=&cancelled;
 assert(abortCheck());
}
'''.replace('CALLBACK',callback)
with tempfile.TemporaryDirectory() as directory:
 path=Path(directory);(path/'test.cpp').write_text(program)
 subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
 subprocess.run([str(path/'test')],check=True)
print('No-data cancel polling, throttle, optional callback, redirect and timer wrap passed')
