#!/usr/bin/env python3
"""Publish a built RickyOS catalog (fonts, standby pictures or the OTA offer) to the RickyOS COS bucket.

Takes the output directory of build_rickyos_font_catalog.py or build_rickyos_wallpapers.py,
built with --cos-base, and uploads with coscli (https://github.com/tencentyun/coscli):

  1. <dir>/<kind>/*  ->  cos://<bucket>/<kind>/<tag>/   immutable, never overwritten
  2. <dir>/cos.json  ->  cos://<bucket>/<kind>/cos.json  no-cache, last

The manifest goes last, so a reader never sees a list whose files are not there yet;
the release folders are immutable, so a reader mid-download keeps its files.

--kind ota takes the flasher site's built dist/ (tools/rickyos-flasher, npm run build) and
mirrors the device's OTA files under cos://<bucket>/ota/, laid out like the site:
  1. dist/<file named by ota.json>  ->  ota/firmware/<name>.bin  immutable, never overwritten
  2. dist/ota-notes.txt              ->  ota/ota-notes.txt         no-cache
  3. dist/ota.json                   ->  ota/ota.json              no-cache, last
The image is checked against ota.json's bytes and sha256 first. --tag is the offered
version. Devices from 1.2.0 ask the bucket first (RickyOtaPolicy.h); older ones only the
site, so every release goes to both.
--device plus publishes the Plus offer instead: dist/plus/ota.json and ota-notes.txt to
ota/plus/, its image to the shared ota/firmware/. The Read Pico's files are untouched.
--cos-dir (default ota) puts everything under another top folder, e.g. ota-test for a
device test build; the official folder is never written then.

Credentials come from the environment, never the command line or the repository:
COS_SECRET_ID, COS_SECRET_KEY, COS_BUCKET (<bucket>-<appid>), COS_REGION (ap-guangzhou)
and optionally COS_SESSION_TOKEN.

Run: python3 scripts/publish_rickyos_assets_cos.py --kind fonts --output DIR --tag v1.1.0 [--coscli PATH] [--dry-run]
"""
import argparse
import hashlib
import json
import re
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rickyos_asset_routes import check_cos_base  # noqa: E402

KINDS = ("fonts", "wallpapers", "ota")
DEVICES = {"readpico": ("", "readpico"), "plus": ("plus/", "metalio-plus-s31")}
IMMUTABLE = "Cache-Control:public,max-age=31536000,immutable"
ROLLING = "Cache-Control:no-cache"


def plan_ota(site, version, bucket, device="readpico", cos_dir="ota"):
    """The OTA copies: image, notes, then the offer; checked against ota.json first."""
    if device not in DEVICES:
        raise ValueError(f"device must be one of {tuple(DEVICES)}")
    if not re.fullmatch(r"ota(-[a-z0-9]+)?", cos_dir):
        raise ValueError(f"unexpected COS folder {cos_dir}")
    sub, board = DEVICES[device]
    offer = json.loads((site / sub / "ota.json").read_text())
    if offer.get("status") != "update_available" or offer.get("version") != version:
        raise ValueError(f"{site / sub / 'ota.json'} does not offer {version}")
    if offer.get("board") != board:
        raise ValueError(f"{site / sub / 'ota.json'} is for {offer.get('board')}, not {board}")
    file = offer["file"]
    if not re.fullmatch(r"firmware/[A-Za-z0-9._-]+\.bin", file):
        raise ValueError(f"unexpected OTA file {file}")
    image = (site / file).read_bytes()
    if len(image) != offer["bytes"] or hashlib.sha256(image).hexdigest() != offer["sha256"]:
        raise ValueError(f"{site / file} does not match ota.json (bytes / sha256)")
    notes = site / sub / "ota-notes.txt"
    if not notes.read_text().startswith(version + "\n"):
        raise ValueError(f"{notes} is not for {version}")
    return [
        (str(site / file), f"cos://{bucket}/{cos_dir}/{file}", ["--forbid-overwrite", "--meta", IMMUTABLE]),
        (str(notes), f"cos://{bucket}/{cos_dir}/{sub}ota-notes.txt", ["--meta", ROLLING]),
        (str(site / sub / "ota.json"), f"cos://{bucket}/{cos_dir}/{sub}ota.json", ["--meta", ROLLING]),
    ]


def plan(kind, output, tag, bucket, region, device="readpico", cos_dir="ota"):
    """The coscli copies to run, in order, after checking the build output."""
    if kind not in KINDS:
        raise ValueError(f"kind must be one of {KINDS}")
    if kind == "ota":
        return plan_ota(output, tag, bucket, device, cos_dir)
    files = output / kind
    manifest = output / "cos.json"
    if not files.is_dir() or not any(files.iterdir()):
        raise ValueError(f"No files in {files}")
    if not manifest.is_file():
        raise ValueError(f"{manifest} is missing: build with --cos-base")
    document = json.loads(manifest.read_text())
    base = document.get("baseUrl", "")
    expected_host = f".cos.{region}.myqcloud.com"
    domain = base.split(f"/{kind}/")[0]
    check_cos_base(domain)
    if domain != f"https://{bucket}{expected_host}" or base != f"{domain}/{kind}/{tag}/":
        raise ValueError(f"cos.json baseUrl {base} does not match bucket {bucket} in {region}, {kind} {tag}")
    return [
        (f"{files}/", f"cos://{bucket}/{kind}/{tag}/", ["--recursive", "--forbid-overwrite", "--meta", IMMUTABLE]),
        (str(manifest), f"cos://{bucket}/{kind}/cos.json", ["--meta", ROLLING]),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--kind", choices=KINDS, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="the build script's output directory (ota: the flasher site's dist/)")
    parser.add_argument("--tag", required=True, help="the release tag the catalog was built with (ota: the version)")
    parser.add_argument("--coscli", default="coscli")
    parser.add_argument("--device", choices=tuple(DEVICES), default="readpico", help="ota: whose offer to publish")
    parser.add_argument("--cos-dir", default="ota", help="ota: the COS top folder (ota-test for device tests)")
    parser.add_argument("--dry-run", action="store_true", help="print the copies without credentials or uploads")
    args = parser.parse_args()

    bucket = os.environ.get("COS_BUCKET", "")
    region = os.environ.get("COS_REGION", "ap-guangzhou")
    if not bucket:
        parser.error("set COS_BUCKET (<bucket>-<appid>) in the environment")
    copies = plan(args.kind, args.output.resolve(), args.tag, bucket, region, args.device, args.cos_dir)
    if args.dry_run:
        for source, target, options in copies:
            print(f"{source} -> {target} {' '.join(options)}")
        return

    secret_id = os.environ.get("COS_SECRET_ID", "")
    secret_key = os.environ.get("COS_SECRET_KEY", "")
    if not secret_id or not secret_key:
        parser.error("set COS_SECRET_ID and COS_SECRET_KEY in the environment")
    credentials = ["--init-skip", "--disable-log", "-i", secret_id, "-k", secret_key,
                   "-e", f"cos.{region}.myqcloud.com"]
    if os.environ.get("COS_SESSION_TOKEN"):
        credentials += ["--token", os.environ["COS_SESSION_TOKEN"]]
    for source, target, options in copies:
        print(f"Uploading {source} -> {target}", flush=True)
        subprocess.run([args.coscli, "cp", *credentials, source, target, *options], check=True)
    if args.kind == "ota":
        print(f"Published: https://{bucket}.cos.{region}.myqcloud.com/{args.cos_dir}/{DEVICES[args.device][0]}ota.json")
    else:
        print(f"Published: https://{bucket}.cos.{region}.myqcloud.com/{args.kind}/cos.json")


if __name__ == "__main__":
    main()
