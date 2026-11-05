"""Verify private credentials are opt-in in actual compile commands, never implicit."""
import json
import os
from pathlib import Path
import shlex
import sys


def check(path, enabled):
    entries = json.loads(path.read_text())
    selected = []
    marker = '-DRICKYOS_PLUS_NET_PRIVATE_PROFILE=1'
    for entry in entries:
        arguments = entry.get('arguments') or shlex.split(entry['command'])
        marked = marker in arguments
        if marked and not enabled:
            raise SystemExit('Private Wi-Fi profile unexpectedly present; do not flash or publish this build')
        if Path(entry['file']).name == 'PlusRadioAuto.cpp':
            selected.append(marked)
    if len(selected) != 1 or selected[0] != enabled:
        raise SystemExit('Private Wi-Fi bootstrap compile mode does not match the explicit request; do not flash')
    print('Actual private network profile compilation verified: enabled=' + str(enabled).lower())


if __name__ == '__main__':
    check(Path(sys.argv[1]), os.environ.get('RICKYOS_PLUS_NET_PRIVATE_PROFILE') == '1')
