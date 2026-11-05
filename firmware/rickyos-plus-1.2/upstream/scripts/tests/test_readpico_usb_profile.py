"""USB 传书 (USB MSC) ships in the one Read Pico product build."""

import configparser
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ReadPicoUsbProfileTest(unittest.TestCase):
    def test_usb_msc_ships_in_the_product_build(self):
        # USB 传书 passed device acceptance for 1.2: one Read Pico firmware carries it,
        # so the product patches (display, SD, keys, TLS) are never left out of it.
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "platformio.ini")
        self.assertNotIn("env:rickyos_readpico_usb", config.sections())
        shipped = config["env:rickyos_readpico"]
        self.assertIn("-DFREEINK_CAP_USB_MSC=1", shipped["build_flags"])
        self.assertIn("CONFIG_TINYUSB_ENABLED=y", shipped["custom_sdkconfig"])
        self.assertIn("CONFIG_TINYUSB_MSC_ENABLED=y", shipped["custom_sdkconfig"])
        self.assertIn("CONFIG_TINYUSB_CDC_ENABLED=n", shipped["custom_sdkconfig"])
        scripts = shipped["extra_scripts"]
        self.assertIn("pre:scripts/prepare_readpico_usb_component.py", scripts)
        self.assertIn("pre:scripts/patch_rickyos_epdiy.py", scripts)
        self.assertIn("-fno-lto", shipped["build_unflags"])
        self.assertIn("-flto=auto", shipped["build_flags"])

    def test_browser_preview_is_simulator_only(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "platformio.ini")
        simulator = config["env:simulator_rickyos"]
        shipped = config["env:rickyos_readpico"]

        self.assertIn("-DRICKYOS_USB_PREVIEW=1", simulator["build_flags"])
        self.assertNotIn("RICKYOS_USB_PREVIEW", shipped["build_flags"])

        activity = (ROOT / "src/activities/network/UsbDriveActivity.cpp").read_text()
        self.assertIn("#if defined(RICKYOS_USB_PREVIEW)", activity)
        self.assertIn("#if !defined(RICKYOS_USB_PREVIEW)\n  if (!restartRequested) Storage.endUsbDrive();", activity)
        self.assertIn("#else\n\n  // Show the safety instructions", activity)

    def test_host_suspend_does_not_eject_mounted_disk(self):
        activity = (ROOT / "src/activities/network/UsbDriveActivity.cpp").read_text()
        self.assertNotIn("Storage.usbDriveHostSuspended()", activity)
        self.assertIn("state == State::Ejected || state == State::Disconnected", activity)

    def test_component_is_pinned_and_untracked(self):
        source = (ROOT / "scripts/prepare_readpico_usb_component.py").read_text()
        self.assertIn("LIB_BUILDER_COMMIT =", source)
        self.assertIn("TINYUSB_COMMIT =", source)
        self.assertIn("hashlib.sha256(contents).hexdigest() != digest", source)
        self.assertIn("if not verify_component(DESTINATION):", source)
        self.assertIn('PROJECT / ".pio" / "rickyos_usb_components"', source)
        self.assertIn('env.BoardConfig().update(', source)


if __name__ == "__main__":
    unittest.main()
