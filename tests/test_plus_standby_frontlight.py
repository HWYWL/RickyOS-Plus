"""Test real standby light lifecycle statements without entering device sleep."""
from pathlib import Path
import subprocess
import tempfile

source = Path('firmware/rickyos-plus-1.2/upstream/src/activities/apps/standby/StandbyActivity.cpp').read_text()
start = source.index('  savedFrontlightOn_ =')
end = source.index('\n#if', start)
enter = source[start:end]
start = source.index('  const bool restoreLight =')
end = source.index('\n  LOG_DBG', start)
leave = source[start:end]
program = r'''
#include <cassert>
#define LOG_INF(...) ((void)0)
struct Light {
 bool available=true, lit=false;
 int brightness=60, warmth=50;
 bool present() const { return available; }
 bool isOn() const { return lit; }
 void setOn(bool on) { lit=on; }
} Frontlight;
struct Settings { int frontlightRestoreOnWake=1, frontlightOn=1; } SETTINGS;
int main() {
 for(bool available : {false,true}) for(bool lit : {false,true}) for(bool restore : {false,true}) {
  bool savedFrontlightOn_=false;
  Frontlight={available,lit,23,87}; SETTINGS={restore,lit};
  ENTER
  assert(!Frontlight.lit);
  assert(Frontlight.brightness==23 && Frontlight.warmth==87 && SETTINGS.frontlightOn==lit);
  LEAVE
  assert(Frontlight.lit==(available && lit && restore));
  assert(Frontlight.brightness==23 && Frontlight.warmth==87 && SETTINGS.frontlightOn==lit);
 }
}
'''.replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>').replace('ENTER', enter).replace('LEAVE', leave)
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root/'standby.cpp').write_text(program)
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',str(root/'standby.cpp'),'-o',str(root/'standby')],check=True)
    subprocess.run([str(root/'standby')],check=True)
print('Standby turns light off, preserves preferences and restores only previous-On + restore-on-wake passed')
