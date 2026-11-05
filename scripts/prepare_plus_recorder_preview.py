"""Prepare an isolated native preview of the real Plus recorder UI, without device I/O."""
from pathlib import Path
import shutil
import subprocess
root = Path(__file__).resolve().parents[1]
source = root / 'firmware/rickyos-plus-1.2/upstream'
generated = root / 'firmware/rickyos-plus-1.2/generated/fonts'
preview = root / '.local/plus-recorder-simulator'
if preview.exists():
    raise SystemExit('Preview already exists; use its build/server commands or choose a fresh checkout.')
if not source.is_dir() or not generated.is_dir():
    raise SystemExit('Restore Plus sources and prepare resources first.')
subprocess.run(['python3', str(root / 'scripts/check_rickyos_plus_sources.py')], check=True)
shutil.copytree(source, preview, ignore=shutil.ignore_patterns('.git', '.pio', '.venv', '__pycache__'))
shutil.copytree(generated, preview / 'preview-generated/fonts')
subprocess.run(['patch', '--batch', '-p1', '-i', str(root / 'scripts/plus-recorder-preview.patch')], cwd=preview, check=True)
print(f'Prepared {preview}; build simulator_rickyos with PlatformIO, then run tools/rickyos-browser/server.py')
