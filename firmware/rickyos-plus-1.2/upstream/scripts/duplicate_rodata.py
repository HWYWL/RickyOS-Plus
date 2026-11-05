#!/usr/bin/env python3
"""Find rodata constants that are genuinely duplicated in the linked firmware.

A file-scope `static const` array referenced by an inline function in a header
gets a private copy in every translation unit that instantiates that function.
With `-fdata-sections` each copy is its own input section, and because the symbol
has *internal* linkage the linker cannot merge the copies: they all survive.

Detecting this from the linker map is unreliable -- the map is written before
identical-section merging and garbage collection, so it lists copies that the
final image does not contain (e.g. `inline constexpr` data, reported many times
in the map but present once). This reads the ELF symbol table instead, so every
row is a copy that really is in the image.

Weak symbols (`V`/`W`, what `inline constexpr` produces) are already merged to a
single instance and are therefore not reported.

Usage:
  scripts/duplicate_rodata.py [--elf PATH] [--top 30] [--min-size 64]
                             [--filter SUBSTR] [--show-origins]
"""

import argparse
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ELF = REPO_ROOT / ".pio" / "build" / "rickyos_readpico" / "firmware.elf"

NM_CANDIDATES = (
    Path.home() / ".platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm",
    Path.home() / ".platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp-elf-nm",
)

# Itanium-mangled internal-linkage symbol: _ZL<length><identifier>.
_LOCAL = re.compile(r"^_ZL(?P<len>\d+)(?P<name>.+)$")
# Object origins from `nm -l`, e.g. "  /abs/path/file.cpp:123".
_SITE = re.compile(r"^\s+(?P<file>\S+):(?P<line>\d+)\s*$")


def find_nm() -> str:
    for candidate in NM_CANDIDATES:
        if candidate.is_file():
            return str(candidate)
    sys.exit("xtensa nm not found; is the PlatformIO toolchain installed?")


def pretty(name: str) -> str:
    match = _LOCAL.match(name)
    if match:
        return match.group("name")[: int(match.group("len"))]
    return name


def read_copies(nm: str, elf: Path):
    """Yield (name, type, size, address) for every defined rodata symbol."""
    proc = subprocess.run(
        [nm, "-l", "--print-size", "--radix=d", str(elf)],
        capture_output=True, text=True, check=True,
    )
    for line in proc.stdout.splitlines():
        parts = line.split("\t", 1)
        fields = parts[0].split(None, 3)
        if len(fields) < 4:
            continue
        address, size_text, type_letter, name = fields
        # 'd'/'D' are the .rodata/.literal data symbols on this target; 'r'/'R'
        # are read-only too. Weak 'V'/'v'/'W'/'w' are already merged.
        if type_letter not in ("d", "D", "r", "R"):
            continue
        try:
            size = int(size_text)
        except ValueError:
            continue
        if size <= 0:
            continue
        yield name, type_letter, size, address


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--elf", default=str(DEFAULT_ELF))
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--min-size", type=int, default=64)
    parser.add_argument("--filter", default=None)
    parser.add_argument("--show-origins", action="store_true")
    args = parser.parse_args()

    elf = Path(args.elf)
    if not elf.is_file():
        sys.exit(f"ELF not found: {elf}\nBuild first: pio run -e rickyos_readpico")

    nm = find_nm()
    # name -> {address: size}; distinct addresses are distinct copies.
    instances: dict[str, dict[str, int]] = defaultdict(dict)
    for name, _type, size, address in read_copies(nm, elf):
        instances[name][address] = size

    rows = []
    for name, by_address in instances.items():
        if len(by_address) < 2:
            continue
        pretty_name = pretty(name)
        if args.filter and args.filter not in pretty_name:
            continue
        size = next(iter(by_address.values()))
        if size < args.min_size:
            continue
        rows.append((size * (len(by_address) - 1), len(by_address), size, pretty_name, name))

    rows.sort(key=lambda r: -r[0])
    total = sum(r[0] for r in rows)

    print(f"elf: {elf}")
    print(f"duplicated constants: {len(rows):,}   reclaimable: {total:,} bytes")
    print()
    if not rows:
        return 0
    print(f"{'waste':>9} {'copies':>7} {'size':>8}  constant")
    print("-" * 92)
    for waste, count, size, pretty_name, _mangled in rows[: args.top]:
        print(f"{waste:>9,} {count:>7} {size:>8,}  {pretty_name}")
    if len(rows) > args.top:
        print(f"... and {len(rows) - args.top:,} more")
    print()
    print(f"total reclaimable: {total:,} bytes across {len(rows):,} constants")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
