"""Mirror the shared game into the pinned Plus snapshot; record reproducibly.

Does not fetch, change the source pin, commit, push, flash, or touch a device.
"""
from pathlib import Path
import argparse
import subprocess
import sys

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
SNAPSHOT = PORT / "firmware/rickyos-plus-1.2/upstream"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline-repo', required=True, type=Path,
                    help='repository containing the pinned source and initialized SDK git checkout')
parser.add_argument('--fonts', action='store_true', help='copy regenerated shared UI font subsets too')
args = parser.parse_args()
if not (args.baseline_repo / 'freeink-sdk/.git').exists():
    raise SystemExit('Baseline repository must have an initialized SDK checkout')
directory = "src/activities/apps/gomoku"
paths = [p.relative_to(ROOT).as_posix() for pattern in ("SkillGomoku*", "GomokuModeActivity*", "GomokuLayout.h", "GomokuArt.h")
         for p in (ROOT / directory).glob(pattern) if p.is_file()]
if not paths:
    raise SystemExit("Shared Skill Gomoku sources missing")
paths.append(f"{directory}/GomokuGameActivity.cpp")
paths.append(f"{directory}/GomokuGameActivity.h")
if args.fonts:
    for name in ('notosans_cjk_14', 'notosans_cjk_16',
                 'ricky_caption_10_regular', 'ricky_title_16_bold', 'ricky_headline_14_medium', 'ricky_display_23_bold'):
        paths.append(f"lib/EpdFont/builtinFonts/{name}.h")
    for name in ('cn_common_chars.txt', 'cn_i18n_chars.txt', 'cn_rickyos_chars.txt'):
        paths.append(f"lib/EpdFont/scripts/{name}")
for name in paths:
    (SNAPSHOT / name).write_bytes((ROOT / name).read_bytes())
# Do not copy the product body's 12pt face: the Plus pin already contains
# these characters with frequency-grouped compression. Its packing stays intact.
manager = SNAPSHOT / "src/activities/ActivityManager.cpp"
text = manager.read_text()
for old, new in [('#include "apps/gomoku/GomokuMenuActivity.h"', '#include "apps/gomoku/GomokuModeActivity.h"'),
                 ('void ActivityManager::goToGomoku() { replaceActivityWith<GomokuMenuActivity>(); }',
                  'void ActivityManager::goToGomoku() { replaceActivityWith<GomokuModeActivity>(); }')]:
    if new not in text:
        if text.count(old) != 1:
            raise SystemExit("Plus game entry changed; inspect before syncing")
        text = text.replace(old, new)
manager.write_text(text)
for language in ("english", "chinese"):
    relative = f"lib/I18n/translations/{language}.yaml"
    additions = [line for line in (ROOT / relative).read_text().splitlines() if line.startswith("STR_SGM_")]
    target = SNAPSHOT / relative
    existing = [line for line in target.read_text().splitlines() if not line.startswith("STR_SGM_")]
    while existing and not existing[-1].strip():
        existing.pop()
    target.write_text("\n".join(existing + [""] + additions) + "\n")
command = [sys.executable, str(PORT / "scripts/sync_plus_design.py"), "--repo", str(args.baseline_repo), "--record"]
for name in paths:
    command.extend(["--add-source", name])
subprocess.run(command, check=True)
