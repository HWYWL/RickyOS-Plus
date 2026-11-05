"""Assert the linked objects route time sync through the Plus clock owner.

Run after a build; the default build directory matches scripts/build_plus.ps1 and
can be overridden with RICKYOS_BUILD_DIR. Paths are compared by file name because
the compile database uses Windows separators on one host and Posix on another.
"""
import json
import os
import shlex
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
build = Path(os.environ.get('RICKYOS_BUILD_DIR', root / '.local/build-rickyos-plus'))
commands = json.loads((build / 'compile_commands.json').read_text())
for filename in ['TrustedTime.cpp', 'WifiSelectionActivity.cpp']:
    entry = next(x for x in commands if Path(x['file']).name == filename)
    args = shlex.split(entry['command'], posix=False)
    compiler = Path(args[0].strip('"'))
    nm = compiler.with_name(compiler.name.replace('g++', 'nm'))
    obj = Path(entry['directory']) / args[args.index('-o') + 1].strip('"')
    symbols = subprocess.check_output([str(nm), '-u', '-C', str(obj)]).decode()
    if filename == 'TrustedTime.cpp':
        assert 'HalClock::requestSync()' in symbols and 'HalClock::syncNow(' in symbols, symbols
        for forbidden in ['configTzTime', 'sntp_set_time_sync_notification_cb', 'sntp_get_sync_status']:
            assert forbidden not in symbols, forbidden
    else:
        assert 'HalClock::requestSync()' in symbols, symbols
        assert 'HalClock::syncNow(' not in symbols, symbols
    print(filename + ': linked object uses the Plus clock owner; legacy SNTP calls absent')
