"""Home key gestures live outside the base settings list, so they need their own save/load."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HomeButtonPersistenceTest(unittest.TestCase):
    def test_gestures_are_saved_and_loaded(self):
        lists = (ROOT / "src/SettingsList.h").read_text()
        base = lists.split("getBaseSettingsList()", 1)[1].split("getSettingsList(", 1)[0]
        self.assertNotIn("home_button::", base)  # still menu-only, so persistence must be explicit
        source = (ROOT / "src/CrossPointSettings.cpp").read_text()
        save = source.split("void CrossPointSettings::toJson(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("doc[home_button::KEYS[i]] = s.*home_button::FIELDS[i]", save)
        load = source.split("void CrossPointSettings::toJson(", 1)[1].split("\n}\n", 1)[1]
        self.assertIn("doc[home_button::KEYS[i]] | fallback", load)
        # The explicit load must precede the legacy long-press migration, which only
        # fires when the new key is absent.
        self.assertLess(load.index("doc[home_button::KEYS[i]] | fallback"),
                        load.index('doc["homeButtonLongPressAction"].isNull()'))


if __name__ == "__main__":
    unittest.main()
