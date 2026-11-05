"""Copy real Plus reader sources into an isolated desktop preview (no device IO).

The desktop HAL remains the existing Read Pico 684x1216 simulator; this is an
application/format test, not an S31 driver, PSRAM budget, or waveform emulator.
Only the disposable preview receives the explicit platform compatibility edits.
"""
from pathlib import Path
import argparse
import shutil
import subprocess

root=Path(__file__).resolve().parents[1]
source=root/'firmware/rickyos-plus-1.2/upstream'
port=root/'firmware/rickyos-plus-1.2/components/rickyos_application/port'
parser=argparse.ArgumentParser();parser.add_argument('--resume',action='store_true')
parser.add_argument('--name', default='plus-kindle-simulator', help='isolated preview directory name')
parser.add_argument('--samples',nargs='*',type=Path,default=[]);args=parser.parse_args()
if Path(args.name).name != args.name or args.name in ('.', '..'):
    raise SystemExit('Preview name must be a single directory name')
preview=root/'.local'/args.name
if preview.exists() and not args.resume:
    raise SystemExit('Preview exists; rebuild there, or choose a fresh preview path.')
subprocess.run(['python3',str(root/'scripts/check_rickyos_plus_sources.py')],check=True)
if not preview.exists():
    shutil.copytree(source,preview,ignore=shutil.ignore_patterns('.git','.pio','.venv','__pycache__'))
    shutil.copytree(root/'firmware/rickyos-plus-1.2/generated/fonts',preview/'preview-generated/fonts')
else:
    # Refresh only the feature integration inputs. Preserve this preview's state
    # card, build cache and explicit desktop adapters.
    for relative in ('lib/Epub/Epub.cpp','lib/Epub/Epub.h','lib/FsHelpers/FsHelpers.cpp',
                     'lib/LibraryIndex/LibraryBuilder.cpp','src/network/CrossPointWebServer.cpp',
                     'src/activities/home/FileBrowserActivity.cpp','src/components/RickyPageUi.h',
                     'src/activities/library/LibraryListActivity.cpp','src/activities/reader/EpubReaderActivity.cpp'):
        shutil.copy2(source/relative,preview/relative)
    shutil.copytree(source/'src/activities/apps/gomoku',preview/'src/activities/apps/gomoku',dirs_exist_ok=True)
    relative='src/activities/ActivityManager.cpp'
    shutil.copy2(source/relative,preview/relative)
    for language in ('english', 'chinese'):
        relative=f'lib/I18n/translations/{language}.yaml'
        shutil.copy2(source/relative,preview/relative)
    shutil.copytree(root/'firmware/rickyos-plus-1.2/generated/fonts',preview/'preview-generated/fonts',dirs_exist_ok=True)
library=preview/'lib/Kindle';library.mkdir(exist_ok=True)
for name in ('KindleDocument.h','KindleDocument.cpp','KindleImport.h','KindleImport.cpp','LICENSE.foliate-mobi','kindle-source.json'):
    shutil.copy2(port/name,library/name)
(preview/'include').mkdir(exist_ok=True)
for name in ('PlusReaderImageLayout.h',):
    shutil.copy2(port/name,preview/'include'/name)
shutil.copy2(root/'scripts/KindlePreviewAudio.cpp',preview/'src/platform/KindlePreviewAudio.cpp')

def replace(relative,old,new):
    path=preview/relative;text=path.read_text()
    if new in text:return
    if text.count(old)!=1:raise RuntimeError(f'Preview adapter context changed: {relative}')
    path.write_text(text.replace(old,new))

replace('platformio.ini','  -DRICKYOS_PRODUCT=1\n  -DRICKYOS_USB_PREVIEW=1',
        '  -DRICKYOS_PRODUCT=1\n  -DRICKYOS_KINDLE_IMPORT=1\n  -Ipreview-generated\n  -DRICKYOS_USB_PREVIEW=1')
replace('platformio.ini','  post:scripts/patch_rickyos_simulator.py\nbuild_src_filter',
        '  post:scripts/patch_rickyos_simulator.py\n  post:scripts/patch_kindle_preview_clock.py\nbuild_src_filter')
shutil.copy2(root/'scripts/kindle_preview_clock.py',preview/'scripts/patch_kindle_preview_clock.py')
replace('src/activities/util/FrontlightPanelActivity.cpp','std::abs(y - 16)','(y < 16 ? 16 - y : y - 16)')
replace('lib/WeReadWebApi/src/WeReadClient.cpp','#include <PlusSha256.h>','#include <mbedtls/sha256.h>')
replace('src/network/HttpDownloader.cpp','if (onSource) onSource(http.getHost().c_str(), sourceContext);',
        'if (onSource) onSource("desktop-preview", sourceContext);')
for old,new in [('PlusSha256Context','mbedtls_sha256_context'),('plus_sha256_','mbedtls_sha256_')]:
    path=preview/'lib/WeReadWebApi/src/WeReadClient.cpp';path.write_text(path.read_text().replace(old,new))
replace('tools/rickyos-browser/index.html','先看效果，<br>再上实机。',
        '技能五子棋<br>玩法测试。' if 'skill-gomoku' in args.name else 'MOBI / AZW3<br>阅读测试。')
replace('tools/rickyos-browser/index.html','MindReset Read Pico · 原生分辨率','Plus 阅读引擎 · 桌面格式验证')
if args.samples:
    import importlib.util
    spec=importlib.util.spec_from_file_location('kindle_preview_server',preview/'tools/rickyos-browser/server.py')
    server=importlib.util.module_from_spec(spec);spec.loader.exec_module(server)
    sd=root/'.local/kindle-preview-state/sd';server.seed_demo(sd)
    for sample in args.samples:
        target=sd/'books'/sample.name
        if target.exists():
            if target.read_bytes()!=sample.read_bytes():raise RuntimeError('Preview sample exists with different contents')
        else:shutil.copy2(sample,target)
print(preview)
