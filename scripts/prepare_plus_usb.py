#!/usr/bin/env python3
"""Restore pinned S31 TinyUSB sources; SdFat owns mounting, so omit esp MSC/FatFS glue."""
from pathlib import Path
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]
base = ROOT/'.local/usb-dependencies'
lock = json.loads((ROOT/'firmware/rickyos-plus-1.2/components/plus_usb/dependency-lock.json').read_text())
repo=base/'esp-usb'
if not repo.exists():
    repo.parent.mkdir(parents=True,exist_ok=True)
    subprocess.run(['git','init',str(repo)],check=True)
    subprocess.run(['git','-C',str(repo),'fetch','--depth=1','https://github.com/espressif/esp-usb.git',lock['esp_usb_commit']],check=True)
    subprocess.run(['git','-C',str(repo),'checkout','--detach','FETCH_HEAD'],check=True)
actual=subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip()
if actual!=lock['esp_usb_commit']:raise SystemExit('Unexpected esp-usb revision')
tusb=base/'tinyusb'
if not tusb.exists():
    from idf_component_tools.sources.web_service import WebServiceSource
    from idf_component_tools.manifest.models import SolvedComponent
    src=WebServiceSource(registry_url='https://components.espressif.com')
    src.download(SolvedComponent(name='espressif/tinyusb',version=lock['version'],component_hash=lock['component_hash'],source=src),str(tusb))
from idf_component_tools.hash_tools.validate import validate_hash_eq_hashdir
validate_hash_eq_hashdir(str(tusb),lock['component_hash'])
source=(repo/'device/esp_tinyusb/tinyusb.c').read_text()
for old in ['#include "tinyusb_msc.h"', '#include "msc_storage.h"','    msc_storage_mount_to_usb();','    msc_storage_mount_to_app();']:
    if source.count(old)!=1:raise SystemExit('Unexpected TinyUSB raw-storage seam')
    source=source.replace(old,'/* Plus: raw storage handoff is owned by HalStorage, never FatFS. */')
(base/'tinyusb.raw.c').write_text(source)
print('Pinned S31 TinyUSB sources verified; raw SdFat handoff prepared')
