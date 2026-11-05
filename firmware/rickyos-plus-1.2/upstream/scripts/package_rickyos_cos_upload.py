#!/usr/bin/env python3
"""Lay out a published RickyOS catalog for upload to the COS bucket by hand (web console).

Reads a checkout of github.com/chinoryunqin/RickyOS-fonts or RickyOS-wallpapers at a
release, checks every listed file against its manifest (size and CRC32), and writes the
bucket layout the firmware reads (src/network/RickyCos.h):

  <output>/<kind>/<tag>/<files>   the release's files
  <output>/<kind>/cos.json        the jsDelivr manifest with baseUrl on COS and the
                                  jsDelivr, Fastly and GitHub routes as mirrors

Upload the <kind> folder to the bucket root in the COS console ("上传文件夹"), keeping
its structure. For new releases prefer build --cos-base + publish_rickyos_assets_cos.py.

Run: python3 scripts/package_rickyos_cos_upload.py --catalog ../RickyOS-fonts --kind fonts --output out/cos
"""
import argparse
import binascii
import json
from pathlib import Path
import re
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rickyos_asset_routes import check_cos_base  # noqa: E402

DEFAULT_COS_BASE = "https://rickyos-1309217881.cos.ap-guangzhou.myqcloud.com"


def listed_files(kind, manifest):
    """(file name, size, crc32) for every file the manifest lists."""
    if kind == "fonts":
        return [(f["name"], f["size"], f["crc32"]) for family in manifest["families"] for f in family["files"]]
    return [(item["file"], item["size"], item["crc32"]) for item in manifest["items"]]


def package(catalog, kind, output, cos_base):
    check_cos_base(cos_base)
    mirror = json.loads((catalog / "mirror.json").read_text())
    match = re.search(rf"@(v[0-9][^/]*)/{kind}/$", mirror.get("baseUrl", ""))
    if not match:
        raise ValueError(f"mirror.json baseUrl is not a jsDelivr {kind} release: {mirror.get('baseUrl')}")
    tag = match.group(1)
    target = output / kind / tag
    if target.exists():
        raise ValueError(f"{target} exists; choose a new output directory")
    target.mkdir(parents=True)
    source = catalog / kind
    for name, size, crc in listed_files(kind, mirror):
        if "/" in name or "\\" in name:
            raise ValueError(f"Unsafe file name: {name}")
        data = (source / name).read_bytes()
        if len(data) != size or (binascii.crc32(data) & 0xFFFFFFFF) != crc:
            raise ValueError(f"{name} does not match its manifest (size or CRC32)")
        (target / name).write_bytes(data)
    for license_file in sorted(source.glob("*.txt")):  # OFL texts travel with the fonts
        shutil.copy2(license_file, target / license_file.name)
    cos = dict(mirror)
    cos["baseUrl"] = f"{cos_base}/{kind}/{tag}/"
    cos["mirrors"] = [mirror["baseUrl"], *mirror.get("mirrors", [])]
    (output / kind / "cos.json").write_text(json.dumps(cos, ensure_ascii=False, indent=1) + "\n")
    return tag, sum(1 for _ in target.iterdir())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--catalog", type=Path, required=True, help="checkout of the catalog repository")
    parser.add_argument("--kind", choices=("fonts", "wallpapers"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cos-base", default=DEFAULT_COS_BASE)
    args = parser.parse_args()
    tag, count = package(args.catalog.resolve(), args.kind, args.output.resolve(), args.cos_base)
    print(f"{args.kind} {tag}: {count} files -> {args.output / args.kind}")


if __name__ == "__main__":
    main()
