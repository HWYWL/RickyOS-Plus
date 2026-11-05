"""Compile the real SSD1677 driver against a recording bus."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
DISPLAY = HERE.parents[1]


def run_trace(source=None, includes=(), defines=(), sources=()):
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        for name in ("driver", "lut"):
            shutil.copytree(DISPLAY / "src" / name, tmp / name)
        shutil.copytree(HERE / "ssd1677_stubs", tmp, dirs_exist_ok=True)
        shutil.copy2(DISPLAY / "include/GrayscaleCapabilities.h", tmp / "GrayscaleCapabilities.h")
        panel = tmp / "driver/PanelDriver.h"
        panel.write_text(panel.read_text().replace("../../include/GrayscaleCapabilities.h", "../GrayscaleCapabilities.h"))
        command = ["c++", "-std=c++17", "-I", str(tmp), "-I", str(HERE),
                   "-I", str(DISPLAY / "include"), "-I",
                   str(DISPLAY.parents[1] / "hardware/BoardConfig/include")]
        for directory in includes:
            command += ["-I", str(directory)]
        command += ["-D" + define for define in defines]
        binary = tmp / "trace"
        command += [str(source or HERE / "test_ssd1677.cpp")]
        command += [str(tmp / path) for path in sources]
        command += ["-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


class Ssd1677Test(unittest.TestCase):
    def test_command_sequences(self):
        run_trace()

    def test_reader_transitions(self):
        run_trace(defines=("FREEINK_SSD1677_READER_TRANSITIONS=1",))


if __name__ == "__main__":
    unittest.main()
