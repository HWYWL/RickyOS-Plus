"""RickyOS online font catalog: subset planning, manifest limits and firmware wiring."""
import importlib.util
from pathlib import Path
import re
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("catalog", ROOT / "scripts/build_rickyos_font_catalog.py")
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)


def regular_only():
    header = struct.pack("<8sHHB19s", b"CPFONT\0\0", 4, 1, 1, bytes(19))
    toc = struct.pack("<B3xIIBhhHHBBBI4x", 0, 1, 1, 3, 2, -1, 0, 0, 0, 0, 0, 64)
    section = struct.pack("<III", 65, 65, 0) + struct.pack("<BBHhhH2xI", 1, 1, 16, 0, 1, 1, 0) + b"\xc0"
    return header + toc + section


class RickyFontCatalogTest(unittest.TestCase):
    def test_regular_only_files_validate(self):
        self.assertEqual([s["style"] for s in catalog.validate_cpfont(regular_only(), {65}, count=1)], [0])
        with self.assertRaises(ValueError):
            catalog.validate_cpfont(regular_only())  # the SD pack still requires real bold

    def test_common_han_covers_simplified_and_traditional(self):
        han = catalog.common_han()
        self.assertEqual(len(han), 8837)
        self.assertTrue({ord(c) for c in "亨佚臺灣們讀"} <= han)

    def test_gaps_are_filled_smallest_first_and_only_with_glyphs_the_font_has(self):
        han = {0x4E00, 0x4E02, 0x4E05, 0x4E09, 0x4E10}
        cmap = set(range(0x4E00, 0x4E11)) - {0x4E07}
        original = catalog.MAX_INTERVALS
        catalog.MAX_INTERVALS = 3
        try:
            intervals, covered = catalog.plan_intervals(cmap, han)
            self.assertEqual(intervals, [[0x4E00, 0x4E05], [0x4E09, 0x4E09], [0x4E10, 0x4E10]])
            self.assertNotIn(0x4E07, covered)
            catalog.MAX_INTERVALS = 1  # 0x4E06-0x4E08 holds a missing glyph
            with self.assertRaises(ValueError):
                catalog.plan_intervals(cmap, han)
        finally:
            catalog.MAX_INTERVALS = original

    def test_a_fragmented_family_drops_only_optional_runs(self):
        original = catalog.MAX_INTERVALS
        try:
            # Four Han runs with holes; ASCII is one more run. 0x4E10 is essential.
            cmap = {cp: "g" for cp in list(range(0x20, 0x7F)) + [0x4E00, 0x4E02, 0x4E04, 0x4E10]}
            han = {0x4E00, 0x4E02, 0x4E04, 0x4E10}
            catalog.MAX_INTERVALS = 3
            with self.assertRaises(ValueError):
                catalog.plan_intervals(cmap, han)  # no essential set: refuse as before
            intervals, covered = catalog.plan_intervals(cmap, han, essential={0x4E10})
            self.assertEqual(len(intervals), 3)
            self.assertIn(0x4E10, covered)
            self.assertTrue(set(range(0x20, 0x7F)) <= covered)  # punctuation and Latin stay
        finally:
            catalog.MAX_INTERVALS = original

    def test_families_fit_the_firmware_manifest_and_licenses(self):
        names = [family["name"] for family in catalog.FAMILIES]
        self.assertEqual(names, ["RickySans", "RickySerif", "LXGWWenKai", "RickyGrin", "RickyBookSong",
                                 "ZhuqueFangsong", "RickyRound"])
        for family in catalog.FAMILIES:
            self.assertRegex(family["name"], r"^[A-Za-z0-9_-]{1,64}$")
            self.assertNotRegex(family["name"], "Source|Smiley")  # OFL Reserved Font Names
            self.assertNotRegex(family["name"], "(?i)LXGW.*Neo|ZhiSong")  # IPA: not the original's name
            if family.get("license_kind") == "IPA":
                self.assertIn("notice", family)  # how to get the original back
            self.assertTrue(0 < len(family["description"].encode()) <= 160)
            self.assertRegex(family["font_sha256"], r"^[0-9a-f]{64}$")
        # The download list draws descriptions in the built-in UI font, which lacks rarer Han.
        ui_font = set((ROOT / "lib/EpdFont/scripts/cn_rickyos_chars.txt").read_text())
        for family in catalog.FAMILIES:
            missing = [c for c in family["description"] if "\u3400" <= c <= "\u9fff" and c not in ui_font]
            self.assertEqual(missing, [], family["name"])
        self.assertLessEqual(catalog.MAX_INTERVALS, 4096)
        self.assertEqual(catalog.FLASH_CACHE_LIMIT, 0x640000 - 4096)

    def test_large_print_sizes_stay_within_reader_limits(self):
        sizes = list(catalog.SIZES)
        self.assertEqual(sizes, sorted(set(sizes)))
        self.assertEqual(catalog.FLASH_SIZES, (12, 14, 16, 18, 20, 22))
        self.assertEqual(max(sizes), 32)
        download = (ROOT / "src/activities/settings/FontDownloadActivity.cpp").read_text()
        self.assertIn(f"kMaxFilesPerFamily = {16}", download)
        self.assertLessEqual(len(sizes), 16)
        self.assertLessEqual(len(sizes) * len(catalog.FAMILIES), 128)  # kMaxManifestFiles
        self.assertIn("kMaxFontFileBytes = 25 * 1024 * 1024", download)
        self.assertEqual(catalog.MAX_FILE_BYTES, 25 * 1024 * 1024)
        # Vector faces reach the same large-print ceiling; their size picker scrolls past 16.
        vector = (ROOT / "src/ReaderFontSizes.h").read_text()
        listed = re.search(r"VECTOR_READER_POINT_SIZES\[\] = \{([^}]*)\}", vector).group(1)
        points = [int(v) for v in listed.replace("\n", " ").split(",")]
        self.assertEqual(points, sorted(set(points)))
        self.assertEqual(max(points), 32)
        popup = (ROOT / "src/components/OptionPopup.h").read_text()
        activate = popup[popup.index("void activate("):popup.index("bool active = false;")]
        self.assertIn("hasMainTabs() ? total", activate)

    def test_wrong_source_stops_before_output_creation(self):
        with tempfile.TemporaryDirectory(prefix="ricky-font-catalog-") as directory:
            sources = Path(directory) / "sources"
            sources.mkdir()
            (sources / catalog.FAMILIES[0]["font"]).write_bytes(b"not the pinned font")
            output = Path(directory) / "output"
            with self.assertRaises(ValueError):
                catalog.build(sources, output, "v1.0.0")
            self.assertFalse(output.exists())

    def test_firmware_reads_the_catalog_and_falls_back(self):
        header = (ROOT / "src/network/RickyFontCatalog.h").read_text()
        repo = catalog.RELEASE_REPO
        self.assertIn(f'"https://github.com/{repo}/releases/latest/download/fonts.json"', header)
        self.assertIn(f'"https://cdn.jsdelivr.net/gh/{repo}@latest/mirror.json"', header)
        activity = (ROOT / "src/activities/settings/FontDownloadActivity.cpp").read_text()
        block = re.search(r"  // RickyOS lists its own catalog.*?\n  \}\n", activity, re.S).group(0)
        self.assertIn("if (purpose_ != Purpose::ReaderAutoInstall)", block)  # NotoSansSC stays on CrossMux
        self.assertIn("RickyFontCatalog::manifestFor(attempt)", block)  # one order, whatever the region
        self.assertIn("if (result == HttpDownloader::OK) break;", activity)
        self.assertIn(f'"https://fastly.jsdelivr.net/gh/{repo}@latest/mirror.json"', header)
        # The Guangzhou bucket leads in China once configured; an unset route is skipped.
        self.assertIn("MANIFEST_ORDER[MANIFEST_COUNT] = {COS_MANIFEST, MIRROR_MANIFEST, FASTLY_MANIFEST,", header)
        self.assertNotIn("contentProfile", header)
        self.assertIn("if (url == nullptr) continue;", activity)

    def test_failed_files_retry_on_mirrors(self):
        script = (ROOT / "scripts/build_rickyos_font_catalog.py").read_text()
        self.assertIn('routes("fonts", RELEASE_REPO, tag, cos_base)', script)
        activity = (ROOT / "src/activities/settings/FontDownloadActivity.cpp").read_text()
        self.assertIn('for (JsonVariant mirror : doc["mirrors"].as<JsonArray>())', activity)
        # Only network failures retry; cancel, SD card and authorization errors stay final.
        self.assertIn("attempt < kFileAttempts && result == HttpDownloader::HTTP_ERROR", activity)
        self.assertIn("const std::string& base = host == 0 ? baseUrl_ : mirrorUrls_[host - 1];", activity)

    def test_product_tcp_window_lifts_the_overseas_throughput_cap(self):
        ini = (ROOT / "platformio.ini").read_text()
        env = ini[ini.index("[env:rickyos_readpico]"):]
        env = env.split("\n[", 1)[0]
        self.assertIn("${readpico_hardware.custom_sdkconfig}", env)
        self.assertIn("CONFIG_LWIP_TCP_WND_DEFAULT=32768", env)
        self.assertIn("CONFIG_LWIP_TCP_RECVMBOX_SIZE=32", env)
        self.assertIn("CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y", ini)  # window buffers prefer PSRAM


if __name__ == "__main__":
    unittest.main()
