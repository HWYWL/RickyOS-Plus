"""RickyOS asset catalogs on the Guangzhou COS bucket: manifest routes, publish order, firmware switch."""
import json
import re
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import package_rickyos_cos_upload as packager  # noqa: E402
import publish_rickyos_assets_cos as publish  # noqa: E402
from rickyos_asset_routes import check_cos_base, routes  # noqa: E402

COS = "https://rickyos-1250000000.cos.ap-guangzhou.myqcloud.com"
REPO = "chinoryunqin/RickyOS-fonts"


class AssetRoutesTest(unittest.TestCase):
    def test_without_cos_the_routes_are_unchanged(self):
        self.assertEqual(routes("fonts", REPO, "v1.1.0"), [
            ("fonts.json", f"https://github.com/{REPO}/releases/download/v1.1.0/",
             [f"https://cdn.jsdelivr.net/gh/{REPO}@v1.1.0/fonts/", f"https://fastly.jsdelivr.net/gh/{REPO}@v1.1.0/fonts/"]),
            ("mirror.json", f"https://cdn.jsdelivr.net/gh/{REPO}@v1.1.0/fonts/",
             [f"https://fastly.jsdelivr.net/gh/{REPO}@v1.1.0/fonts/", f"https://github.com/{REPO}/releases/download/v1.1.0/"]),
        ])

    def test_cos_gets_its_own_manifest_and_joins_every_mirror_list(self):
        manifests = {name: (base, mirrors) for name, base, mirrors in routes("wallpapers", REPO, "v1.3.0", COS)}
        self.assertEqual(set(manifests), {"wallpapers.json", "mirror.json", "cos.json"})
        cos = f"{COS}/wallpapers/v1.3.0/"
        self.assertEqual(manifests["cos.json"][0], cos)
        self.assertNotIn(cos, manifests["cos.json"][1])
        self.assertEqual(manifests["mirror.json"][1][0], cos)  # China's fallback list tries COS next
        self.assertEqual(manifests["wallpapers.json"][1][-1], cos)  # last abroad
        for base, mirrors in manifests.values():
            self.assertTrue(all(url.startswith("https://") and url.endswith("/") for url in [base, *mirrors]))

    def test_cos_base_is_the_bucket_domain(self):
        self.assertEqual(check_cos_base(COS), COS)
        for bad in (COS + "/", "http://a.cos.ap-guangzhou.myqcloud.com", "https://cdn.example.com",
                    "https://a.cos.ap-guangzhou.myqcloud.com.evil.com"):
            with self.assertRaises(ValueError):
                check_cos_base(bad)


class PublishTest(unittest.TestCase):
    def build_output(self, directory, base):
        output = Path(directory)
        (output / "fonts").mkdir()
        (output / "fonts" / "RickySans_12.cpfont").write_bytes(b"x")
        (output / "cos.json").write_text(json.dumps({"version": 1, "baseUrl": base, "mirrors": []}))
        return output

    def test_files_are_immutable_and_the_manifest_goes_last(self):
        with tempfile.TemporaryDirectory() as directory:
            output = self.build_output(directory, f"{COS}/fonts/v1.1.0/")
            copies = publish.plan("fonts", output, "v1.1.0", "rickyos-1250000000", "ap-guangzhou")
        self.assertEqual([target for _, target, _ in copies],
                         ["cos://rickyos-1250000000/fonts/v1.1.0/", "cos://rickyos-1250000000/fonts/cos.json"])
        self.assertIn("--forbid-overwrite", copies[0][2])
        self.assertIn(publish.IMMUTABLE, copies[0][2])
        self.assertIn(publish.ROLLING, copies[1][2])

    def test_a_manifest_for_another_bucket_or_tag_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            output = self.build_output(directory, f"{COS}/fonts/v1.0.0/")
            with self.assertRaises(ValueError):
                publish.plan("fonts", output, "v1.1.0", "rickyos-1250000000", "ap-guangzhou")
            with self.assertRaises(ValueError):
                publish.plan("fonts", output, "v1.0.0", "other-1250000000", "ap-guangzhou")

    def test_credentials_stay_out_of_the_script(self):
        source = (ROOT / "scripts/publish_rickyos_assets_cos.py").read_text()
        self.assertIn('os.environ.get("COS_SECRET_KEY"', source)
        self.assertNotIn("--secret", source)


class ConsolePackageTest(unittest.TestCase):
    def catalog(self, directory, data=b"picture"):
        root = Path(directory) / "catalog"
        (root / "wallpapers").mkdir(parents=True)
        (root / "wallpapers" / "00-rest.png").write_bytes(b"picture")
        import binascii
        manifest = {"version": 1, "baseUrl": "https://cdn.jsdelivr.net/gh/x/RickyOS-wallpapers@v1.3.0/wallpapers/",
                    "mirrors": ["https://github.com/x/RickyOS-wallpapers/releases/download/v1.3.0/"],
                    "items": [{"name": "休息一下", "file": "00-rest.png", "size": len(data),
                               "crc32": binascii.crc32(data) & 0xFFFFFFFF}]}
        (root / "mirror.json").write_text(json.dumps(manifest, ensure_ascii=False))
        return root

    def test_package_is_the_bucket_layout_with_cos_first(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "out"
            tag, count = packager.package(self.catalog(directory), "wallpapers", output, COS)
            self.assertEqual((tag, count), ("v1.3.0", 1))
            self.assertTrue((output / "wallpapers" / "v1.3.0" / "00-rest.png").is_file())
            cos = json.loads((output / "wallpapers" / "cos.json").read_text())
        self.assertEqual(cos["baseUrl"], f"{COS}/wallpapers/v1.3.0/")
        self.assertEqual(cos["mirrors"][0], "https://cdn.jsdelivr.net/gh/x/RickyOS-wallpapers@v1.3.0/wallpapers/")
        self.assertEqual(cos["items"][0]["name"], "休息一下")

    def test_a_file_that_does_not_match_its_manifest_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                packager.package(self.catalog(directory, data=b"other"), "wallpapers", Path(directory) / "out", COS)


class FirmwareSwitchTest(unittest.TestCase):
    def test_cos_route_points_at_the_guangzhou_bucket_and_can_be_switched_off(self):
        header = (ROOT / "src/network/RickyCos.h").read_text()
        base = re.search(r'#define RICKYOS_COS_BASE "([^"]*)"', header).group(1)
        self.assertEqual(check_cos_base(base), "https://rickyos-1309217881.cos.ap-guangzhou.myqcloud.com")
        self.assertIn("#ifndef RICKYOS_COS_BASE", header)
        self.assertIn("ENABLED = sizeof(RICKYOS_COS_BASE) > 1", header)
        for catalog in ("RickyFontCatalog.h", "RickyWallpaperCatalog.h"):
            text = (ROOT / "src/network" / catalog).read_text()
            self.assertIn("RickyCos::ENABLED ?", text)
            self.assertIn(": nullptr;", text)


    def test_ota_mirrors_the_site_image_notes_then_offer(self):
        import hashlib
        with tempfile.TemporaryDirectory() as folder:
            site = Path(folder)
            (site / "firmware").mkdir()
            image = b"RickyOS image" * 100
            (site / "firmware/RickyOS-1.2.0.bin").write_bytes(image)
            (site / "ota-notes.txt").write_text("1.2.0\n更快\n")
            offer = {"board": "readpico", "status": "update_available", "version": "1.2.0",
                     "file": "firmware/RickyOS-1.2.0.bin", "bytes": len(image),
                     "sha256": hashlib.sha256(image).hexdigest()}
            (site / "ota.json").write_text(json.dumps(offer))
            copies = publish.plan("ota", site, "1.2.0", "rickyos-1309217881", "ap-guangzhou")
            self.assertEqual([target for _, target, _ in copies],
                             ["cos://rickyos-1309217881/ota/firmware/RickyOS-1.2.0.bin",
                              "cos://rickyos-1309217881/ota/ota-notes.txt",
                              "cos://rickyos-1309217881/ota/ota.json"])  # the offer goes last
            self.assertIn("--forbid-overwrite", copies[0][2])
            self.assertIn(publish.ROLLING, copies[2][2])
            with self.assertRaises(ValueError):
                publish.plan("ota", site, "1.2.1", "rickyos-1309217881", "ap-guangzhou")  # not what it offers
            (site / "firmware/RickyOS-1.2.0.bin").write_bytes(image + b"x")
            with self.assertRaises(ValueError):
                publish.plan("ota", site, "1.2.0", "rickyos-1309217881", "ap-guangzhou")  # not the hashed image

    def test_plus_offer_goes_beside_the_read_pico_and_tests_stay_out_of_ota(self):
        import hashlib
        with tempfile.TemporaryDirectory() as folder:
            site = Path(folder)
            (site / "firmware").mkdir(); (site / "plus").mkdir()
            image = b"RickyOS Plus image" * 100
            (site / "firmware/RickyOS-Plus-1.2.0.bin").write_bytes(image)
            (site / "plus/ota-notes.txt").write_text("1.2.0\n首个版本\n")
            offer = {"board": "metalio-plus-s31", "status": "update_available", "version": "1.2.0",
                     "file": "firmware/RickyOS-Plus-1.2.0.bin", "bytes": len(image),
                     "sha256": hashlib.sha256(image).hexdigest()}
            (site / "plus/ota.json").write_text(json.dumps(offer))
            copies = publish.plan("ota", site, "1.2.0", "rickyos-1309217881", "ap-guangzhou", "plus")
            self.assertEqual([target for _, target, _ in copies],
                             ["cos://rickyos-1309217881/ota/firmware/RickyOS-Plus-1.2.0.bin",
                              "cos://rickyos-1309217881/ota/plus/ota-notes.txt",
                              "cos://rickyos-1309217881/ota/plus/ota.json"])
            test = publish.plan("ota", site, "1.2.0", "rickyos-1309217881", "ap-guangzhou", "plus", "ota-test")
            self.assertTrue(all(target.startswith("cos://rickyos-1309217881/ota-test/") for _, target, _ in test))
            with self.assertRaises(ValueError):
                publish.plan("ota", site, "1.2.0", "rickyos-1309217881", "ap-guangzhou", "plus", "fonts")
            (site / "ota.json").write_text(json.dumps(offer))  # a Plus offer in the Read Pico's place
            with self.assertRaises(ValueError):
                publish.plan("ota", site, "1.2.0", "rickyos-1309217881", "ap-guangzhou")


if __name__ == "__main__":
    unittest.main()
