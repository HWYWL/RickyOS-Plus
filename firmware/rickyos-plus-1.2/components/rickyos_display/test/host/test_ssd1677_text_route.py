"""Compile the real facade and drivers; validate routing, faults and original image traces."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
LIB = HERE.parents[1]


class TextRoutingTest(unittest.TestCase):
    def test_compile_gate(self):
        include = LIB.parents[1] / "hardware/BoardConfig/include"
        cases = [(board, "ESP32S3", 1, 1) for board in
                 ("STICKY", "X4PRO", "X4CLASSIC", "MURPHY_M4", "WAVESHARE_EPAPER_397", "METALIO_EINK4")]
        cases += [("STICKY", "ESP32S3", 0, 0), ("X4", "ESP32C3", 1, 0),
                  ("EEGO_A4", "ESP32S3", 1, 0), ("PAPERMONO", "ESP32S3", 1, 0)]
        cases += [(board, "ESP32S3", None, int(board not in ("X4PRO", "X4CLASSIC"))) for board in
                  ("STICKY", "X4PRO", "X4CLASSIC", "MURPHY_M4", "WAVESHARE_EPAPER_397", "METALIO_EINK4")]
        with tempfile.TemporaryDirectory() as directory:
            stub = Path(directory)
            (stub / "driver").mkdir()
            (stub / "driver/gpio.h").write_text("")
            (stub / "esp_rom_sys.h").write_text("")
            for board, target, enabled, expected in cases:
                with self.subTest(board=board, enabled=enabled):
                    result = subprocess.run(
                        ["c++", "-std=c++17", "-E", "-dM", "-x", "c++", "-",
                         "-I"+str(include), "-I"+str(stub), "-I"+str(HERE / "pro_stubs"),
                         "-I"+str(LIB / "src/lut"),
                         f"-DFREEINK_DEVICE_{board}=1", f"-DCONFIG_IDF_TARGET_{target}=1",
                         *([] if enabled is None else [f"-DFREEINK_SSD1677_COMBINED_AA={enabled}"])],
                        input='#include "Ssd1677CombinedAa.h"\n#if FREEINK_SSD1677_READER_TRANSITIONS\n#define TEST_TRANSITIONS_ENABLED 1\n#else\n#define TEST_TRANSITIONS_ENABLED 0\n#endif\n#if FREEINK_SSD1677_TEXT_TURN_AA\n#define TEST_TEXT_TURN_ENABLED 1\n#else\n#define TEST_TEXT_TURN_ENABLED 0\n#endif\n', text=True, capture_output=True, check=True)
                    self.assertIn(f"#define FREEINK_SSD1677_TEXT_ROUTING {expected}\n", result.stdout)
                    transitions = int(bool(expected) and board in
                                      ("STICKY", "MURPHY_M4", "METALIO_EINK4", "WAVESHARE_EPAPER_397"))
                    self.assertIn(f"#define TEST_TRANSITIONS_ENABLED {transitions}\n", result.stdout)
                    self.assertIn(f"#define TEST_TEXT_TURN_ENABLED {transitions}\n", result.stdout)

    def test_board_matrix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shutil.copytree(LIB / "src", root / "src")
            shutil.copytree(LIB / "include", root / "include")
            shutil.copytree(HERE / "pro_stubs", root, dirs_exist_ok=True)
            shutil.copy2(HERE / "pro_stubs/EpdBus.h", root / "src/bus/EpdBus.h")
            bus = root / "src/bus/EpdBus.h"
            text = bus.read_text().replace("  unsigned waits=0;", "  unsigned waits=0, activation=0, failAt=0, resets=0; bool busy=false, sleepBusyUntilReset=false, resetKeepsBusy=false, resetClearsFailure=false;")
            text = text.replace("writes.push_back({c,{}});", "assert(!busy); writes.push_back({c,{}}); if (c==0x20) { busy=true; ++activation; } if (c==0x10 && sleepBusyUntilReset) busy=true;")
            text = text.replace("void reset(uint16_t=0) {}", "void reset(uint16_t=0) { ++resets; if (resetClearsFailure) failAt=0; if (!resetKeepsBusy) busy=false; }")
            text = text.replace("++waits;", "++waits; if (!sleepBusyUntilReset || !busy || writes.empty() || writes.back().command != 0x10) busy=failAt && activation==failAt;")
            text = text.replace("return false;", "return busy;")
            bus.write_text(text)
            shutil.copy2(HERE / "ssd1677_stubs/esp_heap_caps.h", root / "esp_heap_caps.h")
            shutil.copy2(LIB.parents[1] / "hardware/BoardConfig/include/MurphyM4Batch.h", root / "MurphyM4Batch.h")
            board = root / "BoardConfig.h"
            board.write_text(board.read_text().replace("XteinkX4Pro,", "XteinkX4Pro, XteinkX4Classic, MurphyM4, PaperMono,"))
            arduino = root / "Arduino.h"
            arduino.write_text(arduino.read_text() + '\n#define DRAM_ATTR\n')
            cases = [(device, False, True) for device in
                     ("STICKY", "X4PRO", "X4CLASSIC", "MURPHY_M4", "WAVESHARE_EPAPER_397", "METALIO_EINK4", "PAPERMONO")]
            cases += [(device, True, True) for device in
                      ("METALIO_EINK4", "STICKY", "MURPHY_M4", "WAVESHARE_EPAPER_397")]
            cases += [(device, False, False) for device in ("X4PRO", "X4CLASSIC")]
            cases = [(*case, False) for case in cases]
            cases += [(board, True, True, None) for board in
                      ("METALIO_EINK4", "STICKY", "MURPHY_M4", "WAVESHARE_EPAPER_397")]
            cases += [(board, False, True, None) for board in ("X4PRO", "X4CLASSIC", "PAPERMONO")]
            for device, transition, combined, edges in cases:
                with self.subTest(device=device, transition=transition, edges=edges):
                    local = device in ("MURPHY_M4", "WAVESHARE_EPAPER_397", "METALIO_EINK4")
                    drivers = ["Ssd1677", "PaperMono", "Uc8179", "Uc8279X4"]
                    if local:
                        drivers += ["CrossMuxSsd1677"]
                    command = ["c++", "-std=c++17", "-DARDUINO=1", "-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1",
                               f"-DFREEINK_SSD1677_TEXT_ROUTING={int(combined and device != 'PAPERMONO')}", "-DFREEINK_DRIVER_SSD1677=1",
                               "-DFREEINK_DRIVER_PAPER_MONO=1", "-DFREEINK_DRIVER_UC8179=1",
                               "-DFREEINK_DRIVER_UC8279_X4=1", f"-DFREEINK_DEVICE_{device}=1",
                               "-I"+str(root), "-I"+str(root / "include"),
                               str(HERE / "test_ssd1677_text_route.cpp"), str(root / "src/FreeInkDisplay.cpp")]
                    command += [str(root / f"src/driver/{name}Driver.cpp") for name in drivers]
                    if transition:
                        command += ["-DFREEINK_SSD1677_READER_TRANSITIONS=1"]
                    if edges is not None:
                        command += [f"-DFREEINK_SSD1677_TEXT_TURN_AA={int(edges)}"]
                    binary = root / "test"
                    subprocess.run(command + ["-o", str(binary)], check=True)
                    scenarios = ["normal", *map(str, range(1, 9))]
                    if device != "PAPERMONO":
                        scenarios += ["rotated", "uc8179", "uc8279"]
                    for scenario in scenarios:
                        subprocess.run([str(binary), scenario], check=True)
                    if device == "MURPHY_M4":
                        subprocess.run([str(binary), "batch1"], check=True)


if __name__ == "__main__":
    unittest.main()
