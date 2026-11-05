#!/usr/bin/env python3
"""Build the online font catalog that RickyOS downloads ("在线下载字体").

Each family becomes regular-only cpfont v4 files that cover common Chinese
(GB2312, Big5 level 1, the 3500 common list) plus Latin and punctuation. Like
the CrossMux catalog, every everyday size (FLASH_SIZES) stays under the reader's
Flash acceleration cache limit, so pages render from flash instead of streaming
glyphs from SD. Large-print sizes are too big for it and stream from SD; a large
page holds few glyphs, so that stays quick.

Sources are pinned by URL and SHA256 and checked before conversion. Converted
families never use an OFL Reserved Font Name ("Source", "Smiley", "得意黑") or, for
the IPA-licensed LXGW Neo ZhiSong, any part of the original's name.
The output directory is the layout of the RickyOS-fonts repository: fonts/
holds the files, fonts.json points at the GitHub release assets of --tag and
mirror.json at the same files through jsDelivr, both in the manifest format
FontDownloadActivity reads; each lists the other hosts under "mirrors". Upload fonts/*.cpfont and fonts.json to the release.
With --cos-base, cos.json serves the same files from the RickyOS COS bucket in Guangzhou, the
first route in mainland China; publish it with scripts/publish_rickyos_assets_cos.py.
"""
import argparse
import binascii
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from build_rickyos_font_pack import FLASH_CACHE_LIMIT, sha, validate_cpfont  # noqa: E402
from rickyos_asset_routes import check_cos_base, routes  # noqa: E402

SCRIPTS = ROOT / "lib/EpdFont/scripts"
FLASH_SIZES = (12, 14, 16, 18, 20, 22)
# Large print, every step the reader offers above 22 pt (VECTOR_READER_POINT_SIZES): 32 pt
# is 67 px on Read Pico, about 9 characters a line. RickySans measures 10.1 MB at 32 pt,
# well under the reader's 25 MB download limit (kMaxFontFileBytes in FontDownloadActivity.cpp).
LARGE_SIZES = (24, 26, 28, 30, 32)
MAX_FILE_BYTES = 25 * 1024 * 1024
SIZES = FLASH_SIZES + LARGE_SIZES
MANIFEST_VERSION = 1  # FONTS_MANIFEST_VERSION in the firmware
MAX_INTERVALS = 4000  # the reader accepts 4096; CrossMux's catalog uses 4000
RELEASE_REPO = "chinoryunqin/RickyOS-fonts"
# Latin, general punctuation, common symbols, CJK punctuation and kana, full-width forms.
NON_HAN = ((0x0020, 0x007E), (0x00A0, 0x00FF), (0x0100, 0x017F), (0x02C6, 0x02DD),
           (0x2000, 0x206F), (0x2070, 0x209F), (0x20A0, 0x20CF), (0x2100, 0x218F),
           (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2460, 0x24FF), (0x2500, 0x257F),
           (0x25A0, 0x25FF), (0x2600, 0x26FF), (0x3000, 0x303F), (0x3040, 0x30FF),
           (0xFE30, 0xFE4F), (0xFF00, 0xFFEF), (0xFFFD, 0xFFFD))
ADOBE = "https://raw.githubusercontent.com/adobe-fonts"
FAMILIES = (
    {"name": "RickySans", "description": "思源黑体 · 清晰耐看的无衬线正文字体",
     "origin": "Source Han Sans CN 2.005 (Adobe)",
     "font": "SourceHanSansCN-Regular.otf",
     "font_url": f"{ADOBE}/source-han-sans/a4f7cf94edfb9d7ffbdfc4841de276358bd7e0f2/SubsetOTF/CN/SourceHanSansCN-Regular.otf",
     "font_sha256": "e2bc8a2e7f37474b774fff8db758681ece40bb6947a90d571bce9dd60671a8e4",
     "license": "LICENSE-Sans.txt",
     "license_url": f"{ADOBE}/source-han-sans/a4f7cf94edfb9d7ffbdfc4841de276358bd7e0f2/LICENSE.txt",
     "license_sha256": "fcac737e761ec63dbfbdce11030a1780161920d80315edba9c8beff1c2bac5a2"},
    {"name": "RickySerif", "description": "思源宋体 · 适合小说长文的衬线字体",
     "origin": "Source Han Serif CN 2.003 (Adobe)",
     "font": "SourceHanSerifCN-Regular.otf",
     "font_url": f"{ADOBE}/source-han-serif/7889f11bf31170b5d092a083b357c8c8130f89e0/SubsetOTF/CN/SourceHanSerifCN-Regular.otf",
     "font_sha256": "3754ea669c530e2473354f8f6d9f79680a44d7e26ec7d00eeabee4a7e0753c5d",
     "license": "LICENSE-Serif.txt",
     "license_url": f"{ADOBE}/source-han-serif/7889f11bf31170b5d092a083b357c8c8130f89e0/LICENSE.txt",
     "license_sha256": "9ff5bb567e1b92c801fc1069e5fbf992ff8efccacb9db94e5959a5b3ba9bb903"},
    {"name": "LXGWWenKai", "description": "LXGW 文楷 · 温润的楷体风格，适合散文诗词",
     "origin": "LXGW WenKai v1.522",
     "font": "LXGWWenKai-Regular.ttf",
     "font_url": "https://github.com/lxgw/LxgwWenKai/releases/download/v1.522/LXGWWenKai-Regular.ttf",
     "font_sha256": "39ad71264b588165b469e35e6afb162a378dacd1f95348160240ba9038ac3009",
     "license": "OFL-WenKai.txt",
     "license_url": "https://raw.githubusercontent.com/lxgw/LxgwWenKai/v1.522/OFL.txt",
     "license_sha256": "c38b1994a5e48ac30ac7d1da7d0409fd8fd8127dfe28a13d6e787d5b1ef34a5e"},
    # The upstream zip is pinned; its Oblique TTF is the only cut the family has.
    {"name": "RickyGrin", "description": "基于得意黑 · 俏皮的斜体黑体，仅含简体常用字",
     "origin": "Smiley Sans v2.0.1 (atelierAnchor)",
     "font": "smiley/SmileySans-Oblique.ttf",
     "font_url": "https://github.com/atelier-anchor/smiley-sans/releases/download/v2.0.1/smiley-sans-v2.0.1.zip",
     "archive": "smiley-sans-v2.0.1.zip",
     "font_sha256": "299c0be6c960ae37361762eca76f7d0cd516615435bb96c0d4b98a1e70178a07",
     "license": "OFL-SmileySans.txt",
     "license_url": "https://raw.githubusercontent.com/atelier-anchor/smiley-sans/v2.0.1/LICENSE",
     "license_sha256": "9401f4050f1b66c26b6ccdc8b0e14a3c1cc37aac122eda84386f25854a9bec72"},
    # IPA Font License 1.0: a derived font's program, font and file names must not contain
    # the original's name, ships under the same license, and says how to get the original.
    {"name": "RickyBookSong", "description": "基于 LXGW 新致宋 · 书卷气的宋体，适合长篇阅读",
     "origin": "LXGW Neo ZhiSong v1.067 (LXGW, IPA Font License 1.0)",
     "font": "LXGWNeoZhiSong.ttf",
     "font_url": "https://github.com/lxgw/LxgwNeoZhiSong/releases/download/v1.067/LXGWNeoZhiSong.ttf",
     "font_sha256": "32b398f9c6278c4ed34f413077add2c2f3c84034d463af2b9cb98f12c4c2c6cd",
     "license": "LICENSE-NeoZhiSong.md", "license_kind": "IPA",
     "license_url": "https://raw.githubusercontent.com/lxgw/LxgwNeoZhiSong/v1.067/LICENSE.md",
     "license_sha256": "1483c7de02dcf8b9c54b3b1aacabf7d6bdf32d412ca6724c0292ea68e862d8cf",
     "notice": "RickyBookSong 是 LXGW Neo ZhiSong v1.067 的衍生字体：只把常用汉字、拉丁字母和标点转换为"
               "RickyOS 的 cpfont 位图格式，字形未作改动，按 IPA Font License 1.0 授权。原始字体："
               "https://github.com/lxgw/LxgwNeoZhiSong/releases/tag/v1.067 ，在阅读器的字体设置中"
               "删除本字体并改用原始字体即可替换回去。\n"},
    {"name": "ZhuqueFangsong", "description": "朱雀仿宋 · 古雅清秀的仿宋，适合小说散文",
     "origin": "Zhuque Fangsong v0.212 (TrionesType)",
     "font": "zhuque/ZhuqueFangsong-Regular.ttf",
     "font_url": "https://github.com/TrionesType/zhuque/releases/download/v0.212/ZhuqueFangsong-v0.212.zip",
     "archive": "ZhuqueFangsong-v0.212.zip",
     "font_sha256": "bb8b661a7643d2296a72d9d10530a00949419c4e527fb61783f73c2ba1a8c062",
     "license": "OFL-Zhuque.txt",
     "license_url": "https://raw.githubusercontent.com/TrionesType/zhuque/v0.212/LICENSE.txt",
     "license_sha256": "66cc01ad4df62fca6936431a426104cb32a52ce8da49de9a7d643e02d7659043"},
    # Built on Source Han Sans, so it carries Adobe's Reserved Font Name "Source".
    {"name": "RickyRound", "description": "基于思源柔黑 · 圆润柔和的圆体",
     "origin": "Resource Han Rounded CN v0.990 (CyanoHao)",
     "font": "rhr/ResourceHanRoundedCN-Regular.ttf",
     "font_url": "https://github.com/CyanoHao/Resource-Han-Rounded/releases/download/v0.990/RHR-CN-0.990.7z",
     "archive": "RHR-CN-0.990.7z",
     "font_sha256": "e7005f7b4a7a0b8352d32c4a1358ff47564eb73be7fdb2db00d9f792755e9dc7",
     "license": "OFL-RHR.txt",
     "license_url": "https://raw.githubusercontent.com/CyanoHao/Resource-Han-Rounded/"
                    "be90fee7a031c1297da5a260ccfb91756088d51c/OFL-License.txt",
     "license_sha256": "586a072c53a12f6e4389f0eb6034f9780afcb9f0a49558e9ed299664ab1107d8"},
)


def common_han():
    """GB2312 (both levels), Big5 level 1 and the 3500 common list."""
    chars = set()
    for lead in range(0xB0, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                chars.add(bytes((lead, trail)).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    for name in ("big5_lv1.txt", "cn_common_chars.txt"):
        chars.update((SCRIPTS / name).read_text().strip())
    return {ord(c) for c in chars if 0x3400 <= ord(c) <= 0x9FFF or 0xF900 <= ord(c) <= 0xFAFF}


def essential_han():
    """The simplified core every family must keep whole: GB2312 and the 3500 common list."""
    chars = set()
    for lead in range(0xB0, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                chars.add(bytes((lead, trail)).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    chars.update((SCRIPTS / "cn_common_chars.txt").read_text().strip())
    return {ord(c) for c in chars if 0x3400 <= ord(c) <= 0x9FFF or 0xF900 <= ord(c) <= 0xFAFF}


def to_intervals(codepoints):
    intervals = []
    for cp in sorted(codepoints):
        if intervals and cp == intervals[-1][1] + 1:
            intervals[-1][1] = cp
        else:
            intervals.append([cp, cp])
    return intervals


def plan_intervals(cmap, han, essential=None):
    """Wanted codepoints the font has, with the smallest gaps filled until it fits.

    Only gaps the font fully covers are filled, so the converter never splits
    them again; the extra glyphs are neighbouring, mostly rarer characters.
    A family that misses many traditional characters (Big5) can still be too
    fragmented: given `essential` (codepoints that must stay), the shortest runs
    made only of other characters are dropped until it fits. Families that fit
    after filling are unchanged by this.
    """
    wanted = {cp for first, last in NON_HAN for cp in range(first, last + 1)} | han
    covered = {cp for cp in wanted if cp in cmap}
    intervals = to_intervals(covered)
    gaps = sorted((intervals[i + 1][0] - intervals[i][1] - 1, i) for i in range(len(intervals) - 1)
                  if all(cp in cmap for cp in range(intervals[i][1] + 1, intervals[i + 1][0])))
    fill = len(intervals) - MAX_INTERVALS
    for _, i in gaps[:max(fill, 0)]:
        covered.update(range(intervals[i][1] + 1, intervals[i + 1][0]))
    intervals = to_intervals(covered)
    if len(intervals) > MAX_INTERVALS and essential is not None:
        keep = essential | {cp for first, last in NON_HAN for cp in range(first, last + 1)}
        optional = sorted((b - a, a, b) for a, b in intervals if not any(cp in keep for cp in range(a, b + 1)))
        for _, a, b in optional[:len(intervals) - MAX_INTERVALS]:
            covered.difference_update(range(a, b + 1))
        intervals = to_intervals(covered)
    if len(intervals) > MAX_INTERVALS:
        raise ValueError(f"{len(intervals)} intervals after filling; the reader accepts {MAX_INTERVALS}")
    return intervals, covered


def check_sources(source_dir):
    for spec in FAMILIES:
        for path, expected in ((spec.get("archive", spec["font"]), spec["font_sha256"]),
                               (spec["license"], spec["license_sha256"])):
            if sha((source_dir / path).read_bytes()) != expected:
                raise ValueError(f"Source SHA256 mismatch: {source_dir / path}")
        if "archive" in spec and not (source_dir / spec["font"]).is_file():
            folder = source_dir / Path(spec["font"]).parent
            raise ValueError(f'Extract {spec["archive"]} into {folder} first (bsdtar -xf ... -C {folder})')


def build(source_dir, output, tag, cos_base=None):
    check_cos_base(cos_base)
    check_sources(source_dir)
    if output.exists():
        raise ValueError("Output already exists; choose a new directory, never overwrite a catalog")
    from fontTools.ttLib import TTFont  # font tooling venv only; checks above need none

    han = common_han()
    essential = essential_han()
    required = set(range(0x20, 0x7F))
    (output / "fonts").mkdir(parents=True)
    families, sources = [], []
    for spec in FAMILIES:
        font = source_dir / spec["font"]
        intervals, covered = plan_intervals(TTFont(font, lazy=True).getBestCmap(), han, essential)
        work = output / ".work" / spec["name"]
        work.mkdir(parents=True)
        with (output / ".work" / f'{spec["name"]}.log').open("w") as log:
            subprocess.run([sys.executable, str(SCRIPTS / "fontconvert_sdcard.py"), str(font),
                            "--intervals", ",".join(f"(0x{a:04X}-0x{b:04X})" for a, b in intervals),
                            "--sizes", ",".join(map(str, SIZES)), "--name", spec["name"],
                            "--output-dir", str(work)], stdout=log, stderr=subprocess.STDOUT, check=True)
        files = []
        for size in SIZES:
            name = f'{spec["name"]}_{size}.cpfont'
            data = (work / name).read_bytes()
            styles = validate_cpfont(data, required, count=1)
            if size in FLASH_SIZES and len(data) > FLASH_CACHE_LIMIT:
                raise ValueError(f"{name} is {len(data):,} B, over the {FLASH_CACHE_LIMIT:,} B flash cache")
            if len(data) >= MAX_FILE_BYTES:
                raise ValueError(f"{name} is {len(data):,} B, over the reader's {MAX_FILE_BYTES:,} B download limit")
            shutil.move(work / name, output / "fonts" / name)
            files.append({"name": name, "size": len(data), "crc32": binascii.crc32(data) & 0xFFFFFFFF})
        kind = spec.get("license_kind", "OFL")
        shutil.copy2(source_dir / spec["license"], output / "fonts" / f'{spec["name"]}-{kind}.txt')
        if "notice" in spec:
            (output / "fonts" / f'{spec["name"]}-NOTICE.txt').write_text(spec["notice"])
        families.append({"name": spec["name"], "description": spec["description"],
                         "styles": ["regular"], "files": files})
        sources.append({"family": spec["name"], "origin": spec["origin"],
                        "font": spec["font_url"], "font_sha256": spec["font_sha256"],
                        "license": spec["license_url"], "license_sha256": spec["license_sha256"],
                        "han_glyphs": sum(1 for cp in covered if cp >= 0x3400),
                        "intervals": len(intervals),
                        "largest_file": max(f["size"] for f in files)})
        print(f'{spec["name"]}: {len(intervals)} intervals, {len(covered)} glyphs, '
              f'largest {sources[-1]["largest_file"]:,} B', flush=True)
    shutil.rmtree(output / ".work")
    # "mirrors" carry the same files; the reader retries a failed file on the next one.
    for name, base, mirrors in routes("fonts", RELEASE_REPO, tag, cos_base):
        manifest = {"version": MANIFEST_VERSION, "baseUrl": base, "mirrors": mirrors, "families": families}
        (output / name).write_text(json.dumps(manifest, ensure_ascii=False, indent=1) + "\n")
    (output / "sources.json").write_text(json.dumps(
        {"sizes": SIZES, "flash_cache_limit": FLASH_CACHE_LIMIT, "families": sources},
        ensure_ascii=False, indent=1) + "\n")
    checksums = [f"{sha(f.read_bytes())}  {f.relative_to(output).as_posix()}"
                 for f in sorted(output.rglob("*")) if f.is_file()]
    (output / "SHA256SUMS").write_text("\n".join(checksums) + "\n")
    print(f"Ready: {output}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source-dir", type=Path, default=ROOT / ".cache/rickyos-font-sources")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tag", required=True, help="GitHub release tag the files are uploaded to, e.g. v1.0.0")
    parser.add_argument("--cos-base", help="RickyOS COS bucket domain (https://<bucket>-<appid>.cos.ap-guangzhou."
                        "myqcloud.com); adds cos.json, published with scripts/publish_rickyos_assets_cos.py")
    args = parser.parse_args()
    build(args.source_dir.resolve(), args.output.resolve(), args.tag, args.cos_base)
