"""Reject silently missing test profiles and leaked profiles in ordinary builds."""
from pathlib import Path
import importlib.util
import json
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('private_profile_check', root / 'scripts/check_plus_private_network_profile.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
marker = '-DRICKYOS_PLUS_NET_PRIVATE_PROFILE=1'
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'commands.json'
    for requested in (False, True):
        path.write_text(json.dumps([{'file': 'port/PlusRadioAuto.cpp', 'command': 'c++ ' + (marker if requested else '')}]))
        checker.check(path, requested)
        try:
            checker.check(path, not requested)
        except SystemExit:
            pass
        else:
            raise AssertionError('Compile mode mismatch accepted')
    for entries in ([], [{'file': 'Other.cpp', 'arguments': ['c++', marker]}],
                    [{'file': 'PlusRadioAuto.cpp', 'command': 'c++'},
                     {'file': 'Other.cpp', 'command': 'c++ ' + marker}]):
        path.write_text(json.dumps(entries))
        try:
            checker.check(path, False)
        except SystemExit:
            pass
        else:
            raise AssertionError('Unknown/leaked private profile accepted')
print('Private network profile: explicit enable, stale/missing bootstrap and ordinary-build leakage checks passed')
