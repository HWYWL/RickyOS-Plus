#!/usr/bin/env python3
"""Prepare the real Plus managed-library application for host UI review.

Uses the existing isolated application/Kindle adapters because that is the Plus
release's source graph. Hardware peripherals and S31 refresh timing are NOT
emulated. No device access, release publication, commits or pushes.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

PORT = Path(__file__).resolve().parents[1]
SOURCE = PORT / 'firmware/rickyos-plus-1.2/upstream'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--shared', required=True, type=Path,
                    help='managed-library shared checkout (supplies maintained desktop adapters)')
args = parser.parse_args()
preview = PORT / '.local/plus-library-simulator'
command = [sys.executable, str(PORT / 'scripts/prepare_plus_kindle_preview.py'), '--name', preview.name]
if preview.exists():
    # Restore only the generated host shell before the existing adapter checks
    # its known title anchors. Preserve the private SD state and build cache.
    shutil.copy2(SOURCE / 'tools/rickyos-browser/index.html', preview / 'tools/rickyos-browser/index.html')
    command.append('--resume')
subprocess.run(command, check=True)
paths = (
    'lib/LibraryIndex/LibraryBuilder.h', 'lib/LibraryIndex/LibraryBuilder.cpp',
    'lib/LibraryIndex/LibrarySources.h', 'lib/LibraryIndex/LibrarySources.cpp',
    'lib/LibraryIndex/LibraryCategories.h', 'lib/LibraryIndex/LibraryCategories.cpp',
    'src/activities/library/LibraryCategoriesActivity.h', 'src/activities/library/LibraryCategoriesActivity.cpp',
    'src/activities/library/LibraryFoldersActivity.h', 'src/activities/library/LibraryFoldersActivity.cpp',
    'src/activities/library/LibrarySourcesActivity.h', 'src/activities/library/LibrarySourcesActivity.cpp',
    'src/activities/library/LibraryListActivity.h', 'src/activities/library/LibraryListActivity.cpp',
    'src/activities/home/FileBrowserActivity.h', 'src/activities/home/FileBrowserActivity.cpp',
    'src/activities/home/RickyStorageActivity.h', 'src/activities/home/RickyStorageActivity.cpp',
    'src/util/BootPreparation.cpp',
)
for relative in paths:
    shutil.copy2(SOURCE / relative, preview / relative)
    if (SOURCE / relative).read_bytes() != (preview / relative).read_bytes():
        raise RuntimeError(f'Preview source differs: {relative}')
shutil.copy2(args.shared / 'scripts/patch_rickyos_simulator.py', preview / 'scripts/patch_rickyos_simulator.py')
shutil.copy2(args.shared / 'tools/rickyos-browser/preview_input.py', preview / 'tools/rickyos-browser/preview_input.py')
page = preview / 'tools/rickyos-browser/index.html'
text = page.read_text().replace('MOBI / AZW3<br>阅读测试。', '书库与存储<br>交互预览。')
text = text.replace('Plus 阅读引擎 · 桌面格式验证', 'Metalio Plus · 书库与存储桌面交互验证')
page.write_text(text)
print(preview)
print(f'Verified {len(paths)} managed-library application inputs against the Plus snapshot')
