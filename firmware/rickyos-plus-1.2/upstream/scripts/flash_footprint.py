#!/usr/bin/env python3
"""Rank firmware flash consumers by exact size, from the PlatformIO linker map.

The Read Pico build has an explicit flash budget: the OTA app slot is 6.25 MiB
and release policy keeps >=512 KiB free. "What is eating flash" therefore needs a
measured answer, not a guess.

The map file is ground truth here. `nm` letter types are not a reliable proxy on
ESP32-S3 -- `.rodata` is linked into the flash-mapped DROM segment, so `d`/`D`
symbols occupy flash, while `.data`/`.bss` occupy RAM. The map states the output
section each contribution lands in, so this parses that instead and cross-checks
the total against the sizes PlatformIO reports.

Flash-resident output sections counted:
  .flash.text  .flash.rodata  .flash.appdesc

Usage:
  scripts/flash_footprint.py --by archive    # biggest libraries / groups
  scripts/flash_footprint.py --by object     # biggest object files
  scripts/flash_footprint.py --by dir        # roll objects up to directories
  scripts/flash_footprint.py --by section    # text vs rodata split

Options:
  --map PATH    firmware.map (default: .pio/build/rickyos_readpico/firmware.map)
  --top N       rows to print (default 30)
  --section     restrict to one output section, e.g. .flash.rodata
"""

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MAP = REPO_ROOT / ".pio" / "build" / "rickyos_readpico" / "firmware.map"

FLASH_SECTIONS = (".flash.text", ".flash.rodata", ".flash.appdesc")

# Map layout, as emitted by GNU ld through the ESP-IDF link:
#
#   .flash.text     0x42000020   0x295c10          <- output section header
#                0x42000020                        _stext = .
#    .literal._ZN7TwoWire5writeEh                    <- input section label (own line)
#                   0x42000020       0x1c .pio/.../Wire.cpp.o   <- address, size, owner
#    .text._ZN7TwoWire5writeEh
#                   0x4200003c       0x58 .pio/.../Wire.cpp.o
#
# The label line carries no size, so the parser stays "armed" by it and reads the
# following address line as the contribution.
_SECTION_HEADER = re.compile(r"^(\.flash\.\S+)\s+0x[0-9a-fA-F]+\s+0x[0-9a-fA-F]+\s*$")
_INPUT_LABEL = re.compile(r"^\s+\.\S+\s*$")
_ADDR_LINE = re.compile(
    r"^\s+0x[0-9a-fA-F]+\s+"
    r"(?:0x(?P<size>[0-9a-fA-F]+)\s+)?"
    r"(?P<origin>\S.*?)\s*$"
)
# "0x1c (size before relaxing)" carries no new bytes.
_ANNOTATION = re.compile(r"^\s+0x[0-9a-fA-F]+\s+\(.*\)\s*$")


def archive_of(origin: str) -> str:
    """Collapse an object path to its owning library / build group."""
    for marker in (".a(", ".a["):
        if marker in origin:
            return origin.split(marker, 1)[0].rsplit("/", 1)[-1]
    # PlatformIO object paths look like
    #   .pio/build/<env>/lib033/Wire/Wire.cpp.o
    #   .pio/build/<env>/src/activities/reader/EpubReaderActivity.cpp.o
    #   .pio/build/<env>/FrameworkArduino/main.cpp.o
    # Group by the first path component under the env directory.
    marker = ".pio/build/"
    if marker in origin:
        tail = origin.split(marker, 1)[1]
        parts = tail.split("/")
        if len(parts) >= 2:
            return "pio:" + parts[1]
    return origin.rsplit("/", 1)[-1]


def parse(map_path: Path):
    by_object: dict[str, int] = defaultdict(int)
    by_section: dict[str, int] = defaultdict(int)
    current: str | None = None

    with map_path.open(errors="replace") as handle:
        for line in handle:
            header = _SECTION_HEADER.match(line)
            if header:
                current = header.group(1)
                continue
            # Any other top-level header (column 0) closes the flash run.
            # Contribution origins are indented, so they cannot match here.
            if line.startswith(".") and not _INPUT_LABEL.match(line):
                current = None
                continue
            if current is None or _ANNOTATION.match(line) or _INPUT_LABEL.match(line):
                continue
            match = _ADDR_LINE.match(line)
            if not match:
                continue
            origin = match.group("origin")
            # Skip linker-script assignments ("_stext = .", "*libfoo.a:(...)")
            # and flag/metadata lines that are not load inputs with a size.
            if "=" in origin or origin.startswith("*") or origin.endswith(")"):
                continue
            size_text = match.group("size")
            if not size_text or not (".o" in origin or ".a" in origin):
                continue
            size = int(size_text, 16)
            if size <= 0:
                continue
            by_object[origin] += size
            by_section[current] += size

    return by_object, dict(by_section)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--map", default=str(DEFAULT_MAP))
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--by", choices=("archive", "object", "dir", "section"), default="archive")
    parser.add_argument("--section", default=None, help="restrict to one output section")
    args = parser.parse_args()

    map_path = Path(args.map)
    if not map_path.is_file():
        sys.exit(f"map not found: {map_path}\nBuild first: pio run -e rickyos_readpico")

    by_object, by_section = parse(map_path)
    total = sum(by_section.values())
    if not total:
        sys.exit("no flash contributions parsed; map format may have changed")

    print(f"map: {map_path}")
    print(f"flash bytes attributed: {total:,}")
    for name, size in sorted(by_section.items(), key=lambda kv: kv[1], reverse=True):
        print(f"  {size:>12,}  {100.0 * size / total:>6.2f}%  {name}")
    print()

    if args.by == "section":
        return 0

    # Directory rollup: prefer the real source path, which PlatformIO keeps for
    # project objects (.pio/build/<env>/src/... and .../lib<hash>/...).
    by_archive: dict[str, int] = defaultdict(int)
    by_dir: dict[str, int] = defaultdict(int)
    for origin, size in by_object.items():
        by_archive[archive_of(origin)] += size
        rel = origin
        marker = ".pio/build/"
        if marker in origin:
            tail = origin.split(marker, 1)[1]
            parts = tail.split("/")
            # parts[0] is the env name; drop it and the synthetic lib<hash> dir
            # so the row reads like a real source directory.
            parts = parts[1:]
            if parts and parts[0].startswith("lib") and origin.startswith(str(REPO_ROOT)) is False:
                parts = parts[1:]
            rel = "/".join(parts)
        elif origin.startswith(str(REPO_ROOT) + "/"):
            rel = origin[len(str(REPO_ROOT)) + 1 :]
        drop = rel.split("/")
        by_dir[str(Path(*drop[:2])) if len(drop) > 2 else rel] += size

    table, label = {
        "archive": (dict(by_archive), "archive / unit"),
        "object": (by_object, "object file"),
        "dir": (dict(by_dir), "directory"),
    }[args.by]

    ranked = sorted(table.items(), key=lambda kv: kv[1], reverse=True)
    print(f"{'bytes':>12}  {'share':>7}  {label}")
    print("-" * 100)
    for name, size in ranked[: args.top]:
        print(f"{size:>12,}  {100.0 * size / total:>6.2f}%  {name}")
    print()
    print(f"{len(ranked):,} distinct rows, {total:,} bytes attributed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
