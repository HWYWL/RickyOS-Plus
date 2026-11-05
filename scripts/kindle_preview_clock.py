"""Desktop-only compatibility for the Plus clock-cancel application call."""
from pathlib import Path

def patch_clock(path):
    text=path.read_text()
    if 'void cancelSync()' in text:return
    old='  bool requestSync();'
    if text.count(old)!=1:raise RuntimeError('Review desktop clock adapter: HAL changed')
    path.write_text(text.replace(old,old+'\n  void cancelSync() { _syncState = ClockSyncState::Idle; }'))

if 'Import' in globals():
    Import('env')
    patch_clock(Path(env.subst('$PROJECT_LIBDEPS_DIR'))/'simulator_rickyos/simulator/src/HalClock.h')
