#!/usr/bin/env python3
"""Build a licensed, verified SD font pack. Never opens/writes a real device.

Uses the existing cpfont v4 converter, not a second font format. Sources are
pinned to official Adobe releases and checked before conversion. Converted
families deliberately avoid the OFL Reserved Font Name 'Source'.
"""
import argparse
import binascii
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SIZES = (12, 14, 16, 18, 20, 22)
INTERVALS = "latin-ext,cjk,symbols,(0x3400-0x4dbf)"
FLASH_CACHE_LIMIT = 0x640000 - 4096
SOURCES = (
    {"family": "RickySans", "origin": "思源黑体 / Source Han Sans CN",
     "repo": "source-han-sans", "commit": "a4f7cf94edfb9d7ffbdfc4841de276358bd7e0f2",
     "regular": "SourceHanSansCN-Regular.otf", "bold": "SourceHanSansCN-Bold.otf",
     "license": "LICENSE-Sans.txt",
     "hashes": ("e2bc8a2e7f37474b774fff8db758681ece40bb6947a90d571bce9dd60671a8e4",
                "62383707c086a32f3afd5e293f34c7eff64c7fea31f579fdc6cbe34d920519a6",
                "fcac737e761ec63dbfbdce11030a1780161920d80315edba9c8beff1c2bac5a2")},
    {"family": "RickySerif", "origin": "思源宋体 / Source Han Serif CN",
     "repo": "source-han-serif", "commit": "7889f11bf31170b5d092a083b357c8c8130f89e0",
     "regular": "SourceHanSerifCN-Regular.otf", "bold": "SourceHanSerifCN-Bold.otf",
     "license": "LICENSE-Serif.txt",
     "hashes": ("3754ea669c530e2473354f8f6d9f79680a44d7e26ec7d00eeabee4a7e0753c5d",
                "4ee555ae58b3d22f6a95c2c494f2c36b7cccfc1d2224635f6461a03756f0e3c1",
                "9ff5bb567e1b92c801fc1069e5fbf992ff8efccacb9db94e5959a5b3ba9bb903")},
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def validate_cpfont(data, required=(), count=2):
    """Check the actual v4 table, coverage and every glyph's bitmap bounds.

    count=2 expects regular+bold; count=1 a regular-only file."""
    header = 32 + count * 32
    if len(data) < header:
        raise ValueError("truncated header/TOC")
    magic, version, flags, styles, reserved = struct.unpack_from("<8sHHB19s", data)
    if (magic, version, flags, styles, reserved) != (b"CPFONT\0\0", 4, 1, count, bytes(19)):
        raise ValueError(f"not a {count}-style cpfont v4")
    toc = [struct.unpack_from("<B3xIIBhhHHBBBI4x", data, 32 + i * 32) for i in range(count)]
    if [entry[0] for entry in toc] != list(range(count)) or toc[0][-1] != header:
        raise ValueError("invalid style roles/offset")
    result = []
    for i, entry in enumerate(toc):
        style, intervals, glyphs, line_height, asc, desc, kl, kr, kcl, kcr, ligs, offset = entry
        end = toc[i + 1][-1] if i + 1 < count else len(data)
        if not (0 < intervals <= 4096 and 0 < glyphs <= 65536 and 0 < line_height <= 255
                and 0 <= kl <= 4096 and 0 <= kr <= 4096 and offset < end <= len(data)):
            raise ValueError("invalid counts/section range")
        glyph_offset = offset + intervals * 12
        bitmap_offset = glyph_offset + glyphs * 16 + (kl + kr) * 3 + kcl * kcr + ligs * 8
        if bitmap_offset > end:
            raise ValueError("metadata beyond section end")
        coverage = set()
        expected_index, previous = 0, -1
        for j in range(intervals):
            first, last, index = struct.unpack_from("<III", data, offset + j * 12)
            if not (previous < first <= last <= 0xffff and index == expected_index):
                raise ValueError("invalid Unicode interval")
            coverage.update(range(first, last + 1))
            expected_index += last - first + 1
            previous = last
        if expected_index != glyphs or not set(required).issubset(coverage):
            raise ValueError("missing required glyphs/count mismatch")
        for j in range(glyphs):
            width, height, advance, left, top, length, bitmap = struct.unpack_from(
                "<BBHhhH2xI", data, glyph_offset + j * 16)
            if bitmap_offset + bitmap + length > end or length != (width * height + 3) // 4:
                raise ValueError("invalid glyph bitmap")
        result.append({"style": style, "glyphs": glyphs, "intervals": intervals,
                       "line_height_px": line_height, "ascender": asc, "descender": desc})
    return result


def build(source_dir, output):
    # Check every original and license before creating an output directory.
    source_records = []
    for spec in SOURCES:
        for key, expected in zip(("regular", "bold", "license"), spec["hashes"]):
            source = source_dir / spec[key]
            if sha(source.read_bytes()) != expected:
                raise ValueError(f"Source SHA256 mismatch: {source}")
            suffix = "LICENSE.txt" if key == "license" else "SubsetOTF/CN/" + spec[key]
            source_records.append({"file": spec[key], "sha256": expected,
                                   "url": f'https://raw.githubusercontent.com/adobe-fonts/{spec["repo"]}/{spec["commit"]}/{suffix}'})
    if output.exists():
        raise ValueError("Output already exists; choose a new directory, never overwrite a pack")
    required = set(range(32, 127))
    required.update(map(ord, (ROOT / "lib/EpdFont/scripts/cn_common_chars.txt").read_text().strip()))
    output.mkdir(parents=True)
    records = []
    for spec in SOURCES:
        family_dir = output / "fonts" / spec["family"]
        family_dir.mkdir(parents=True)
        shutil.copy2(source_dir / spec["license"], family_dir / "OFL.txt")
        with (output / (spec["family"] + "-conversion.log")).open("w") as log:
            subprocess.run([sys.executable, str(ROOT / "lib/EpdFont/scripts/fontconvert_sdcard.py"),
                            "--regular", str(source_dir / spec["regular"]),
                            "--bold", str(source_dir / spec["bold"]),
                            "--intervals", INTERVALS, "--sizes", ",".join(map(str, SIZES)),
                            "--name", spec["family"], "--output-dir", str(family_dir)],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        for size in SIZES:
            file = family_dir / f'{spec["family"]}_{size}.cpfont'
            data = file.read_bytes()
            records.append({"path": file.relative_to(output).as_posix(), "family": spec["family"],
                            "origin": spec["origin"], "size_pt_at_150dpi": size,
                            "bytes": len(data), "sha256": sha(data),
                            "crc32": f"{binascii.crc32(data) & 0xffffffff:08x}",
                            "flash_cache_eligible": len(data) <= FLASH_CACHE_LIMIT,
                            "styles": validate_cpfont(data, required)})
        print(f'{spec["family"]}: 6 sizes, real Regular/Bold, coverage and bitmap bounds verified', flush=True)
    manifest = {"format": "cpfont-v4", "sizes": SIZES, "sources": source_records,
                "intervals": INTERVALS, "required_glyphs_checked": len(required), "files": records}
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    shutil.copy2(ROOT / "docs/rickyos-font-pack.md", output / "README.md")
    package(output)


def package(output):
    checksums = []
    for file in sorted(output.rglob("*")):
        if file.is_file() and file.name not in ("SHA256SUMS", "RickyOS-Fonts-SD.zip"):
            checksums.append(f"{sha(file.read_bytes())}  {file.relative_to(output).as_posix()}")
    (output / "SHA256SUMS").write_text("\n".join(checksums) + "\n")
    archive = output / "RickyOS-Fonts-SD.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for file in sorted(output.rglob("*")):
            if file.is_file() and file != archive:
                info = zipfile.ZipInfo(file.relative_to(output).as_posix(), (2026, 10, 3, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                bundle.writestr(info, file.read_bytes())
    print(f"Ready: {archive} ({archive.stat().st_size:,} bytes)", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=ROOT / ".cache/rickyos-font-sources")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-and-zip", action="store_true",
                        help="Verify an existing manifest's font hashes, then rebuild metadata/archive only")
    args = parser.parse_args()
    if args.verify_and_zip:
        output = args.output.resolve()
        manifest = json.loads((output / "manifest.json").read_text())
        for record in manifest["files"]:
            file = output / record["path"]
            if not file.resolve().is_relative_to(output) or sha(file.read_bytes()) != record["sha256"]:
                raise ValueError("Manifest file path/hash mismatch")
            validate_cpfont(file.read_bytes())
        package(output)
    else:
        build(args.source_dir.resolve(), args.output.resolve())
