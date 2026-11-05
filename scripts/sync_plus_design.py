#!/usr/bin/env python3
"""Move the Plus port to a newer design commit.

Run from the port directory after restore_rickyos_plus_sources.py. For every file in the
pinned source set: unchanged upstream files take the new design version; files the Plus
port adapted are three-way merged (base = old pin, ours = restored Plus file, theirs = new
pin) with `git merge-file`; files deleted upstream leave the set. New upstream files join
the set when their directory already holds pinned files. Then the manifest, the recorded
adaptations and upstream-plus.patch are rewritten against the new pin.

Conflicts stop the run with the conflicted paths left marked in upstream/; resolve them
and rerun with --resume to finish the bookkeeping.

After editing Plus sources in upstream/, record them (adaptations and patch) against the
current pin with --record.

Usage: python3 scripts/sync_plus_design.py --repo /path/to/RickyOS --to <commit> [--resume]
       python3 scripts/sync_plus_design.py --repo /path/to/RickyOS --sdk-repo /path/to/sdk --record
"""
import argparse
import difflib
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / 'firmware/rickyos-plus-1.2'
UPSTREAM = PROJECT / 'upstream'
MANIFEST = PROJECT / 'source-manifest.json'
CHANGES = PROJECT / 'port-changes.json'
PATCH = PROJECT / 'upstream-plus.patch'
STATE = ROOT / '.local/design-sync-state.json'
SDK = 'freeink-sdk/'


def git(repo, *args, text=False):
    return subprocess.check_output(['git', '-C', str(repo), *args], text=text)


def show(repo, commit, path):
    try:
        return git(repo, 'show', f'{commit}:{path}')
    except subprocess.CalledProcessError:
        return None


def sha(data):
    return hashlib.sha256(data).hexdigest()


def unified_source_patch(before, after, name, existing):
    # Git/patch lines are LF-delimited, not Python's wider set of Unicode line
    # separators. Character lists intentionally have no final newline. difflib
    # does not emit the required no-newline marker: joining its raw output glues
    # the next file's header into the last added line and corrupts restoration.
    def lines(data):
        text = data.decode('utf-8', 'surrogateescape')
        parts = text.split('\n')
        return [line + '\n' for line in parts[:-1]] + ([parts[-1]] if parts[-1] else [])
    diff = difflib.unified_diff(lines(before), lines(after),
                                fromfile=f'a/{name}' if existing else '/dev/null', tofile=f'b/{name}')
    return ''.join(line if line.endswith('\n') else line + '\n\\ No newline at end of file\n'
                   for line in diff)


def stage(repo, old, new, manifest, changes):
    adapted = {item['path']: item for item in changes['changes']}
    status = git(repo, 'diff', '--name-status', '--no-renames', old, new, text=True).splitlines()
    pinned_dirs = {str(Path(p).parent) for p in manifest['files'] if not p.startswith(SDK)}
    files = dict(manifest['files'])
    conflicts = []
    for line in status:
        kind, path = line.split('\t', 1)
        if path.startswith(SDK) or path == 'freeink-sdk':
            continue
        target = UPSTREAM / path
        if kind == 'D':
            if path in files:
                if path in adapted:
                    raise SystemExit(f'Adapted file removed upstream, decide by hand: {path}')
                files.pop(path)
                target.unlink(missing_ok=True)
            continue
        theirs = show(repo, new, path)
        if kind == 'A' and path not in files:
            if str(Path(path).parent) not in pinned_dirs:
                continue
        elif path not in files:
            continue
        files[path] = sha(theirs)
        target.parent.mkdir(parents=True, exist_ok=True)
        if path not in adapted:
            target.write_bytes(theirs)
            continue
        ours = target.read_bytes()
        base = show(repo, old, path) or b''
        work = ROOT / '.local/design-sync-merge'
        work.mkdir(parents=True, exist_ok=True)
        trio = [work / 'ours', work / 'base', work / 'theirs']
        for file, data in zip(trio, (ours, base, theirs)):
            file.write_bytes(data)
        result = subprocess.run(['git', 'merge-file', '-p', '-L', 'plus', '-L', 'old-design', '-L', 'new-design',
                                 *map(str, trio)], capture_output=True)
        if result.returncode < 0 or result.returncode > 127:
            raise SystemExit(f'merge-file failed: {path}')
        target.write_bytes(result.stdout)
        if result.returncode:
            conflicts.append(path)
    return files, conflicts


def finish(repo, new, manifest, changes, files, sdk_repo=None):
    if manifest['commit'] != new:
        manifest['previous_commit'] = manifest['commit']
    manifest['commit'] = new
    manifest['files'] = dict(sorted(files.items()))
    manifest['file_count'] = len(files)
    records, patch = [], []
    names = set(files) | {item['path'] for item in changes['changes']}
    for name in sorted(names):
        if name in manifest.get('snapshot_path_overrides', {}):
            continue
        path = UPSTREAM / name
        if not path.is_file():
            continue
        current = sha(path.read_bytes())
        base = files.get(name)
        if current == base:
            continue
        records.append({'path': name, 'baseline_sha256': base, 'ported_sha256': current})
        if name.startswith(SDK):
            old = show(sdk_repo or Path(repo) / 'freeink-sdk', manifest['sdk_commit'], name[len(SDK):]) if base else None
        else:
            old = show(repo, new, name) if base else None
        if base and (old is None or sha(old) != base):
            raise SystemExit(f'Missing/mismatched pinned source for {name}; supply --sdk-repo for SDK sources')
        patch.append(unified_source_patch(old or b'', path.read_bytes(), name, base is not None))
    changes['baseline'] = new
    changes['changes'] = records
    MANIFEST.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n')
    CHANGES.write_text(json.dumps(changes, indent=2, ensure_ascii=False) + '\n')
    PATCH.write_text(''.join(patch), encoding='utf-8', errors='surrogateescape')
    print(f'Pinned {new[:8]}: {len(files)} files, {len(records)} Plus adaptations')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', required=True, type=Path, help='RickyOS repository holding both commits')
    parser.add_argument('--sdk-repo', type=Path, help='Separate Git repository holding the pinned SDK commit')
    parser.add_argument('--to', help='design commit to pin')
    parser.add_argument('--record', action='store_true', help='record edited Plus sources against the current pin')
    parser.add_argument('--add-source', action='append', default=[],
                        help='register a new restored-source file (requires --record)')
    parser.add_argument('--resume', action='store_true', help='finish after resolving conflicts')
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text())
    changes = json.loads(CHANGES.read_text())
    if args.record:
        known = set(manifest['files']) | {item['path'] for item in changes['changes']}
        for name in args.add_source:
            relative = Path(name)
            if relative.is_absolute() or '..' in relative.parts or not (UPSTREAM / relative).is_file():
                parser.error(f'Invalid restored-source path: {name}')
            if name not in known:
                changes['changes'].append({'path': name, 'baseline_sha256': None,
                                          'ported_sha256': sha((UPSTREAM / relative).read_bytes())})
                known.add(name)
        finish(args.repo, manifest['commit'], manifest, changes, manifest['files'], args.sdk_repo)
        return
    if args.add_source:
        parser.error('--add-source requires --record')
    if not args.to:
        parser.error('--to or --record is required')
    new = git(args.repo, 'rev-parse', args.to, text=True).strip()
    sdk = git(args.repo, 'rev-parse', f'{new}:freeink-sdk', text=True).strip()
    if git(args.repo, 'ls-tree', new, 'freeink-sdk', text=True).startswith('120000'):
        # Design worktrees may carry freeink-sdk as a symlink to the shared checkout; its HEAD is the pin.
        sdk = git(Path(args.repo) / 'freeink-sdk', 'rev-parse', 'HEAD', text=True).strip()
    if sdk != manifest['sdk_commit']:
        raise SystemExit(f'New design pins SDK {sdk[:8]}, the port pins {manifest["sdk_commit"][:8]}; sync the SDK first')
    if args.resume:
        state = json.loads(STATE.read_text())
        if state['new'] != new:
            raise SystemExit('Resume target differs from the staged sync')
        marked = [p for p in state['conflicts'] if b'<<<<<<< plus' in (UPSTREAM / p).read_bytes()]
        if marked:
            raise SystemExit('Still conflicted: ' + ', '.join(marked))
        finish(args.repo, new, manifest, changes, state['files'], args.sdk_repo)
        STATE.unlink()
        return
    files, conflicts = stage(args.repo, manifest['commit'], new, manifest, changes)
    if conflicts:
        STATE.write_text(json.dumps({'new': new, 'files': files, 'conflicts': conflicts}, indent=2) + '\n')
        print('Conflicts to resolve in upstream/:', *conflicts, sep='\n  ')
        sys.exit(1)
    finish(args.repo, new, manifest, changes, files, args.sdk_repo)


if __name__ == '__main__':
    main()
