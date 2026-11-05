"""Exercise the real combined driver with allocation failures and a recording bus."""
import unittest
from test_ssd1677 import HERE, run_trace


class CombinedAaTest(unittest.TestCase):
    def test_text_turn(self):
        for white in (32, 40, 48):
            for delay in (0, 8, 16, 24):
                with self.subTest(white=white, delay=delay):
                    run_trace(HERE / "test_ssd1677_text_turn.cpp", defines=(
                        "FREEINK_SSD1677_TEXT_ROUTING=1", "FREEINK_DEVICE_METALIO_EINK4=1",
                        f"FREEINK_SSD1677_TEXT_WHITE_FRAMES={white}",
                        f"FREEINK_SSD1677_TEXT_BLACK_DELAY={delay}"))

        for board in ("STICKY", "MURPHY_M4", "WAVESHARE_EPAPER_397"):
            with self.subTest(board=board):
                run_trace(HERE / "test_ssd1677_text_turn.cpp", defines=(
                    "FREEINK_SSD1677_TEXT_ROUTING=1", f"FREEINK_DEVICE_{board}=1"))

    def test_sticky(self):
        run_trace(HERE / "test_sticky_combined_aa.cpp",
                  defines=("FREEINK_SSD1677_TEXT_ROUTING=1", "FREEINK_DEVICE_STICKY=1"))

    def test_papermono(self):
        run_trace(HERE / "test_sticky_combined_aa.cpp",
                  defines=("FREEINK_SSD1677_TEXT_ROUTING=0",))

    def test_image_text_image_switch(self):
        run_trace(HERE / "test_sticky_image_route.cpp",
                  defines=("FREEINK_SSD1677_TEXT_ROUTING=1", "FREEINK_DEVICE_STICKY=1"),
                  sources=("driver/Ssd1677Driver.cpp", "driver/PaperMonoDriver.cpp"))


if __name__ == "__main__":
    unittest.main()
