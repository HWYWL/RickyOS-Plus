#!/usr/bin/env python3
"""Check the pinned 1.2 snapshot and its explicitly recorded Plus source edits."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parent.parent / 'firmware/rickyos-plus-1.2'
manifest = json.loads((root / 'source-manifest.json').read_text())
changes = {item['path']: item for item in json.loads((root / 'port-changes.json').read_text())['changes']}
overrides = manifest.get('snapshot_path_overrides', {})
errors = []
for name, baseline in manifest['files'].items():
    path = root / overrides[name] if name in overrides else root / 'upstream' / name
    expected = changes.get(name, {}).get('ported_sha256', baseline)
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        errors.append(name)
for name, change in changes.items():
    if name in manifest['files']:
        continue
    path = root / 'upstream' / name
    if change.get('baseline_sha256') is not None or not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != change['ported_sha256']:
        errors.append(name)
if errors:
    raise SystemExit('Unrecorded source changes: ' + ', '.join(errors))
touch = 'freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp'
if touch not in changes or changes[touch]['ported_sha256'] == manifest['files'][touch]:
    raise SystemExit('Missing Plus shared-bus touch adaptation; generic Wire must not own these pins')
print(f"Checked {len(manifest['files'])} pinned files; {len(changes)} recorded source adaptations")
