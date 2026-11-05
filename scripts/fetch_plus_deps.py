#!/usr/bin/env python3
"""Fetch the pinned Plus third-party libraries without PlatformIO as a build system.

Sources are chosen to reproduce `dependency-lock.json` byte for byte:

* Libraries that the previous PlatformIO build consumed as registry packages are
  downloaded from the PlatformIO package CDN with `urllib` (no `pio` binary, no
  PlatformIO project). Those archives carry the `.piopm` metadata file that the
  lock hashes include, so the check is strict: every recorded file must match.
* Libraries that were plain Git checkouts (JPEGDEC, esp-mdns, Arduino core,
  esp-usb) are fetched by commit SHA with LF line endings pinned, otherwise
  Windows' `core.autocrlf=true` default rewrites the tree and every hash fails.

Resulting layout (git-ignored):

    .local/
      arduino-esp32-4.0.0-RC1/
      dependencies/{Arduino-wolfSSL,ArduinoJson,SdFat,PNGdec,JPEGDEC,
                    QRCode,WebSockets,PubSubClient,mdns}
      usb-dependencies/esp-usb/

TinyUSB itself comes from the ESP component registry and is handled by
`prepare_plus_usb.py`, which needs the ESP-IDF Python environment.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import shutil
import subprocess
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / ".local"
DEPS = LOCAL / "dependencies"
PROJECT = ROOT / "firmware/rickyos-plus-1.2"
LOCK = json.loads((PROJECT / "dependency-lock.json").read_text(encoding="utf-8"))

# name -> (registry owner, version). Downloaded from the PlatformIO package CDN.
REGISTRY_DEPS = [
    ("Arduino-wolfSSL", "wolfssl"),
    ("ArduinoJson", "bblanchon"),
    ("SdFat", "greiman"),
    ("PNGdec", "bitbank2"),
    ("QRCode", "ricmoo"),
    ("WebSockets", "links2004"),
    ("PubSubClient", "knolleary"),
]

# name -> (git url, commit, destination)
GIT_DEPS = [
    ("JPEGDEC", "https://github.com/bitbank2/JPEGDEC.git",
     "86282979224c8a32fd51e091ed5a35b0c699a52b", DEPS / "JPEGDEC", None),
    # mdns lives inside esp-protocols; the component root is components/mdns.
    ("mdns", "https://github.com/espressif/esp-protocols.git",
     "4fb97b209fbc695cc3019e0f1ef7937f766e6503", DEPS / "mdns", "components/mdns"),
    # Arduino-ESP32 4.0.0-RC1: the first core with esp32s31 support.
    ("arduino-esp32-4.0.0-RC1", "https://github.com/espressif/arduino-esp32.git",
     "76f683d935b390f2805301ec10a5562bbbb37811", LOCAL / "arduino-esp32-4.0.0-RC1", None),
    ("esp-usb", "https://github.com/espressif/esp-usb.git",
     "c77046c5d3a6b44c556a95d789bdec914db1cb71", LOCAL / "usb-dependencies" / "esp-usb", None),
]

REGISTRY = "https://api.registry.platformio.org/v3/packages/{owner}/library/{name}"

# The package CDN refuses requests that do not identify as a PlatformIO client.
# This is a download source only: no pio binary and no PlatformIO project is used.
USER_AGENT = "PlatformIO/6.1.16 (RickyOS-Plus dependency fetch)"


def http_get(url: str, timeout: int) -> bytes:
    """Download with retries: the package CDN sometimes truncates a large body."""
    last: Exception | None = None
    for attempt in range(4):
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.read()
        except Exception as error:  # noqa: BLE001 - retry any transport failure
            last = error
            print(f"  retry {attempt + 1}/4 after: {error}")
    raise SystemExit(f"Download failed: {url}\n{last}")


def run(args: list[str], cwd: Path | None = None) -> str:
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f"{' '.join(args)} failed:\n{result.stderr.strip()}")
    return result.stdout


def verify(target: Path, name: str) -> list[str]:
    """Return the recorded files that are missing or hash differently.

    `.piopm` is PlatformIO install metadata, not source: it is rewritten on every
    install, so it is reported but never treated as a failure.
    """
    bad: list[str] = []
    if name not in LOCK:
        # The Arduino core is pinned by commit only; dependency-lock.json has no
        # per-file baseline for it.
        return bad
    for relative, expected in LOCK[name]["files"].items():
        if relative == ".piopm":
            continue
        candidate = target / relative
        if not candidate.is_file():
            bad.append(f"{relative} (missing)")
            continue
        if hashlib.sha256(candidate.read_bytes()).hexdigest() != expected:
            bad.append(relative)
    return bad


def safe_extract(archive: tarfile.TarFile, target: Path) -> None:
    target.mkdir(parents=True, exist_ok=True)
    for member in archive.getmembers():
        destination = (target / member.name).resolve()
        if not str(destination).startswith(str(target.resolve())):
            raise SystemExit(f"Refusing archive path escape: {member.name}")
        if member.isdir():
            destination.mkdir(parents=True, exist_ok=True)
            continue
        if not member.isfile():
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        source = archive.extractfile(member)
        if source is not None:
            destination.write_bytes(source.read())


def fetch_registry(name: str, owner: str, version: str) -> None:
    target = DEPS / name
    packument = json.loads(http_get(REGISTRY.format(owner=owner, name=name), 60))
    entry = next((v for v in packument["versions"] if v["name"] == version), None)
    if entry is None:
        raise SystemExit(f"{name}: registry has no version {version}")
    files = entry.get("files") or []
    if not files:
        raise SystemExit(f"{name}: registry entry has no download")
    download = files[0]
    payload = http_get(download["download_url"], 180)
    digest = hashlib.sha256(payload).hexdigest()
    expected = download.get("checksum", {}).get("sha256")
    if expected and digest != expected:
        raise SystemExit(f"{name}: package checksum mismatch")
    if target.exists():
        shutil.rmtree(target)
    with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive:
        safe_extract(archive, target)
    print(f"  {target.relative_to(ROOT)} <- registry {version} ({len(payload) // 1024} KiB)")


def fetch_git(name: str, url: str, commit: str, target: Path, subdir: str | None = None) -> None:
    checkout = target
    if subdir:
        checkout = LOCAL / "source-cache" / name
    if checkout.exists():
        shutil.rmtree(checkout)
    checkout.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "init", "--quiet", str(checkout)])
    run(["git", "-C", str(checkout), "config", "core.autocrlf", "false"])
    run(["git", "-C", str(checkout), "config", "core.eol", "lf"])
    run(["git", "-C", str(checkout), "fetch", "--depth=1", "--quiet", url, commit])
    run(["git", "-C", str(checkout), "checkout", "--quiet", "--detach", "FETCH_HEAD"])
    revision = run(["git", "-C", str(checkout), "rev-parse", "HEAD"]).strip()
    if subdir:
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(checkout / subdir, target)
    print(f"  {target.relative_to(ROOT)} <- git {revision[:12]}{'/' + subdir if subdir else ''}")


def apply_patch_file(directory: Path, patch: Path) -> None:
    """Apply a git-format patch once; already-patched trees are left untouched."""
    # core.autocrlf=false matters: a registry package is not a Git work tree, so
    # `git apply` would otherwise use the global setting and CRLF the whole file.
    git = ["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf", "apply"]
    reverse = [git + ["--check", "--reverse", str(patch)],
               ["patch", "--binary", "--dry-run", "--reverse", "-p1", "-i", str(patch)]]
    for command in reverse:
        if subprocess.run(command, cwd=directory, capture_output=True).returncode == 0:
            return
    for command in (git + ["--whitespace=nowarn", str(patch)],
                    ["patch", "--binary", "--batch", "--forward", "-p1", "-i", str(patch)]):
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
        if result.returncode == 0:
            return
    raise SystemExit(f"Patch does not apply: {patch.name} in {directory}")


WOLFSSL_MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
WOLFSSL_BLOCK = """/* CrossPoint wolfSSL compatibility overrides */
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
/* MEMFIX-PORT: 8192 handles up to RSA-4096 keys (the public-CA maximum,
   ISRG Root X1 included) with half the per-bignum heap of 16384: with
   WOLFSSL_SMALL_STACK each fast-math temp is FP_MAX_BITS/8 * 2 bytes on the
   heap, and TLS cert verification allocates dozens at once. */
#undef FP_MAX_BITS
#define FP_MAX_BITS 8192"""


def apply_app_patches() -> None:
    """Reproduce the source patches the PlatformIO build applied to its libdeps.

    Upstream owns these patches (`upstream/scripts/*_patches`); the Plus port only
    re-applies them to the fetched trees so the build is byte-identical to the
    reviewed dependency snapshot.
    """
    upstream_scripts = PROJECT / "upstream" / "scripts"

    sd_fat = DEPS / "SdFat"
    if ("SdFat" in LOCK) and sd_fat.is_dir():
        for patch in sorted((upstream_scripts / "sdfat_patches").glob("*.patch")):
            apply_patch_file(sd_fat, patch)
        print(f"SdFat patches applied from {len(list((upstream_scripts / 'sdfat_patches').glob('*.patch')))} files")

    jpeg = DEPS / "JPEGDEC"
    if jpeg.is_dir():
        for patch in sorted((upstream_scripts / "jpegdec_patches").glob("*.patch")):
            apply_patch_file(jpeg, patch)
        print("JPEGDEC patches applied")

    settings = DEPS / "Arduino-wolfSSL" / "src" / "user_settings.h"
    if settings.is_file():
        text = settings.read_bytes().decode("utf-8")
        if WOLFSSL_MARKER in text:
            text = text.split(WOLFSSL_MARKER, 1)[0]
        text = text.rstrip("\r\n")
        # The upstream PlatformIO script appended the block with implicit newline
        # counts. Keep the pinned bytes authoritative: pick the layout whose hash
        # matches dependency-lock.json instead of guessing the blank lines.
        expected = LOCK["Arduino-wolfSSL"]["files"]["src/user_settings.h"]
        for lead, trail in ((2, 2), (2, 1), (1, 2), (1, 1)):
            candidate = (text + "\n" * lead + WOLFSSL_BLOCK + "\n" * trail).encode("utf-8")
            if hashlib.sha256(candidate).hexdigest() == expected:
                settings.write_bytes(candidate)
                print("wolfSSL user_settings.h overrides applied")
                break
        else:
            raise SystemExit("wolfSSL override block cannot reproduce the pinned bytes; re-fetch Arduino-wolfSSL")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", nargs="*", help="fetch just these dependency names")
    parser.add_argument("--skip-arduino", action="store_true",
                        help="skip the Arduino core (~400 MB); app libraries only")
    args = parser.parse_args()
    wanted = set(args.only) if args.only else None
    failures: list[str] = []
    # These three are expected to differ from the lock until the reviewed patches
    # are applied; the post-patch verification below is the authoritative check.
    patched_later = {"SdFat", "JPEGDEC", "Arduino-wolfSSL"}

    for name, owner in REGISTRY_DEPS:
        if wanted and name not in wanted:
            continue
        version = LOCK[name]["version"]
        target = DEPS / name
        if target.is_dir() and not verify(target, name):
            print(f"{name} {version}: already verified, skipping download")
            continue
        print(f"{name} {version}")
        fetch_registry(name, owner, version)
        bad = verify(DEPS / name, name)
        print(f"  {len(LOCK[name]['files'])} files {'verified' if not bad else 'pending patch'}")
        if bad and name not in patched_later:
            failures.append(f"{name}: {len(bad)} hash mismatches ({', '.join(bad[:3])})")

    for name, url, commit, target, subdir in GIT_DEPS:
        if wanted and name not in wanted:
            continue
        if args.skip_arduino and name.startswith("arduino-esp32"):
            print(f"{name}: skipped (--skip-arduino)")
            continue
        if target.is_dir() and (target / ".git").exists() and not verify(target, name):
            print(f"{name} {commit[:12]}: already verified, skipping fetch")
            continue
        print(f"{name} {commit[:12]}")
        fetch_git(name, url, commit, target, subdir)
        bad = verify(target, name)
        print(f"  {len(LOCK[name]['files'])} files {'verified' if not bad else 'pending patch'}")
        if bad and name not in patched_later:
            failures.append(f"{name}: {len(bad)} hash mismatches ({', '.join(bad[:3])})")

    if failures:
        raise SystemExit("Dependency fetch incomplete:\n  " + "\n  ".join(failures))

    print("\nApplying the reviewed dependency patches (SdFat, JPEGDEC, wolfSSL)")
    apply_app_patches()
    for name in ("SdFat", "JPEGDEC", "Arduino-wolfSSL"):
        bad = verify(DEPS / name, name)
        print(f"  {name}: {'verified' if not bad else 'MISMATCH ' + ', '.join(bad[:3])}")
        if bad:
            failures.append(f"{name}: patched tree does not match dependency-lock.json")

    if failures:
        raise SystemExit("Dependency preparation incomplete:\n  " + "\n  ".join(failures))
    print("\nPinned libraries verified against dependency-lock.json.")


if __name__ == "__main__":
    main()
