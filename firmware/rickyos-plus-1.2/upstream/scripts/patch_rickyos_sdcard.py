"""Expose the mounted volume so RickyOS can count free space in slices.

SdFat counts free clusters in one pass over the whole FAT (exFAT: the cluster
bitmap). On a large FAT32 card that is megabytes of reads, and the caller holds
the global SD lock the whole time, so every other page that touches the card
waits. RickyOS reads the same table itself a few sectors at a time and drops the
lock between slices (src/util/RickyFreeSpace.cpp); for that it needs the volume
geometry, which the SDK keeps behind a private accessor.

Like patch_rickyos_epdiy.py: a bounded mechanical rewrite, re-runnable, and the
build fails if the SDK source no longer matches instead of silently skipping it.
"""
from pathlib import Path

MARK = 'RICKYOS_VOLUME_ACCESS'

OLD_DECL = '''  FsBlockDeviceInterface* rawBlockDevice();
'''
NEW_DECL = '''  FsBlockDeviceInterface* rawBlockDevice();
  // RICKYOS_VOLUME_ACCESS: the mounted volume, for geometry reads (fatType,
  // fatStartSector, ...). Caller holds the storage lock.
  FsVolume& mountedVolume() { return vol(); }
'''


def patch_text(source, old, new, mark=MARK):
    if new in source:
        return source
    if source.count(old) != 1:
        raise RuntimeError('SDK SD card source changed; review the RickyOS patch (%s) before building.' % mark)
    return source.replace(old, new)


def apply(project_dir):
    header = Path(project_dir) / 'freeink-sdk/libs/hardware/SDCardManager/include/SDCardManager.h'
    source = header.read_text()
    patched = patch_text(source, OLD_DECL, NEW_DECL)
    if patched != source:
        header.write_text(patched)


if 'Import' in globals():
    Import('env')
    if env.subst('$PIOENV') == 'rickyos_readpico':
        apply(env.subst('$PROJECT_DIR'))
elif __name__ == '__main__':
    apply(Path(__file__).resolve().parents[1])
