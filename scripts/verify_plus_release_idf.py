#!/usr/bin/env python3
"""Fail closed on an unreviewed Plus IDF, before compiling and before packaging.

Two revisions are accepted:

* `9a97f6c54ec638111ce55cd36581b3c192f15207` — release/v6.1 head, the revision the
  current Plus port is reviewed against. Preferred.
* `fff9895c82d744c7237be8847347bdd1b07c6643` — the v6.1 release tag, which earlier
  Plus builds used and which is what the ESP-IDF installer produces by default. It
  also supports `esp32s31`, so an existing installation can be reused instead of
  downloading a second multi-gigabyte toolchain.

Anything else is rejected, and the checkout must have no modified tracked files.
Untracked files are allowed because the IDF installers leave generated files such
as `tools/requirements/requirements.merged.txt` behind.
"""
import argparse
import json
from pathlib import Path
import subprocess

EXPECTED = '9a97f6c54ec638111ce55cd36581b3c192f15207'
RELEASE_TAG = 'fff9895c82d744c7237be8847347bdd1b07c6643'
ACCEPTED = (EXPECTED, RELEASE_TAG)


def verify(idf, project=None):
    idf = Path(idf).resolve(strict=True)
    revision = subprocess.check_output(['git', '-C', str(idf), 'rev-parse', 'HEAD'], text=True).strip()
    if revision not in ACCEPTED:
        raise ValueError('Wrong ESP-IDF ' + revision + '; Plus requires ' + EXPECTED
                         + ' (or the v6.1 tag ' + RELEASE_TAG + ')')
    dirty = subprocess.check_output(['git', '-C', str(idf), 'status', '--porcelain',
                                     '--untracked-files=no'], text=True).strip()
    if dirty:
        raise ValueError('Plus ESP-IDF checkout has modified tracked files; use a clean commit')
    if project is not None:
        description = json.loads(Path(project).read_text())
        revision_field = str(description.get('git_revision') or '')
        accepted_fields = {value for revision_value in ACCEPTED
                           for value in (revision_value, revision_value[:8])}
        # A tag checkout records the tag name ("v6.1") instead of a SHA.
        tag_description = revision == RELEASE_TAG and revision_field.split('-')[0] in ('v6.1', '6.1')
        if revision_field not in accepted_fields and not tag_description:
            raise ValueError('Build description does not identify a reviewed ESP-IDF')
        if Path(description.get('idf_path', '')).resolve() != idf:
            raise ValueError('Build used a different IDF path')
        if description.get('target') != 'esp32s31':
            raise ValueError('Build target is not esp32s31')
    return revision


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--idf', required=True, type=Path)
    parser.add_argument('--project', type=Path)
    args = parser.parse_args()
    try:
        revision = verify(args.idf, args.project)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')
    if revision == EXPECTED:
        print('ESP-IDF ' + EXPECTED + ' verified (preferred)')
    else:
        print('ESP-IDF ' + revision + ' verified (v6.1 release tag; accepted fallback)')
