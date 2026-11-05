#!/usr/bin/env python3
"""Restore the exact Plus application/SDK snapshot, then apply recorded port edits."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / 'firmware/rickyos-plus-1.2'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-repo', type=Path, help='Existing RickyOS Git repository (optional offline source)')
parser.add_argument('--sdk-repo', type=Path, help='Existing FreeInk SDK Git repository (optional offline source)')
args = parser.parse_args()
manifest = json.loads((PROJECT / 'source-manifest.json').read_text())
changes = json.loads((PROJECT / 'port-changes.json').read_text())
assert manifest['commit'] == changes['baseline']
assert manifest['sdk_commit'] == changes['sdk_baseline']
final = PROJECT / 'upstream'
if final.exists():
    subprocess.run([sys.executable, str(ROOT / 'scripts/check_rickyos_plus_sources.py')], check=True)
    raise SystemExit('Existing Plus snapshot verified; no files overwritten')
staging = PROJECT / 'upstream-restore'
if staging.exists():
    raise SystemExit('An incomplete upstream-restore directory exists; inspect it before retrying')
staging.mkdir()
cache = ROOT / '.local/source-cache'
overrides = manifest.get('snapshot_path_overrides', {})
seen = set()
for prefix, supplied, url, commit in [
    ('', args.source_repo, 'https://github.com/chinoryunqin/RickyOS-dev.git', manifest['commit']),
    ('freeink-sdk/', args.sdk_repo, 'https://github.com/0x1abin/freeink-sdk.git', manifest['sdk_commit']),
]:
    repo = supplied.resolve() if supplied else cache / ('sdk' if prefix else 'application')
    if not repo.exists():
        repo.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'init', '--bare', str(repo)], check=True)
    probe = subprocess.run(['git', '-C', str(repo), 'cat-file', '-e', commit + '^{commit}'], capture_output=True)
    if probe.returncode:
        if supplied:
            raise SystemExit(f'Supplied repository lacks pinned commit {commit}')
        subprocess.run(['git', '-C', str(repo), 'fetch', '--depth=1', url, commit], check=True)
    # Force LF on Windows: core.autocrlf would rewrite the archive and break both the
    # pinned hashes and the literal patch context applied below.
    proc = subprocess.Popen(['git', '-c', 'core.autocrlf=false', '-c', 'core.eol=lf',
                             '-C', str(repo), 'archive', commit], stdout=subprocess.PIPE)
    with tarfile.open(fileobj=proc.stdout, mode='r|') as archive:
        for member in archive:
            name = prefix + member.name
            if name not in manifest['files'] or not member.isfile():
                continue
            data = archive.extractfile(member).read()
            if hashlib.sha256(data).hexdigest() != manifest['files'][name]:
                raise SystemExit(f'Pinned source hash mismatch: {name}')
            seen.add(name)
            # Keep case-colliding SDK headers at their recorded separate paths.
            target = PROJECT / overrides[name] if name in overrides else staging / name
            if name in overrides and target.exists():
                if target.read_bytes() != data:
                    raise SystemExit(f'Collision baseline mismatch: {name}')
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            target.chmod(member.mode & 0o777)
    if proc.wait():
        raise SystemExit('git archive failed')
missing = set(manifest['files']) - seen
if missing:
    raise SystemExit('Missing archive sources: ' + ', '.join(sorted(missing)))
subprocess.run(['patch', '--batch', '-p1', '-i', str(PROJECT / 'upstream-plus.patch')], cwd=staging, check=True)
ported = {c['path']: c['ported_sha256'] for c in changes['changes']}
for name, baseline in manifest['files'].items():
    target = PROJECT / overrides[name] if name in overrides else staging / name
    if hashlib.sha256(target.read_bytes()).hexdigest() != ported.get(name, baseline):
        raise SystemExit(f'Restored Plus source mismatch: {name}')
for name, change in ((c['path'], c) for c in changes['changes']):
    if name in manifest['files']:
        continue
    target = staging / name
    if change.get('baseline_sha256') is not None or not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest() != change['ported_sha256']:
        raise SystemExit(f'Restored added source mismatch: {name}')
try:
    staging.rename(final)
except OSError:
    # Windows refuses a directory rename while a handle is still open (antivirus,
    # the patcher process); fall back to a copy-then-delete move.
    import shutil
    shutil.move(str(staging), str(final))
subprocess.run([sys.executable, str(ROOT / 'scripts/check_rickyos_plus_sources.py')], check=True)
print('Pinned Plus sources restored and port patch verified')
