"""Compile the Metalio SDK board profile against a recording Wire bus."""

import pathlib
import subprocess
import tempfile
import unittest


BOARD_INCLUDE = pathlib.Path(__file__).resolve().parents[2] / "include"


class MetalioChargerTest(unittest.TestCase):
    def test_boot_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            (tmp / "Arduino.h").write_text(r'''
#pragma once
#include <cstdint>
constexpr int OUTPUT = 1, INPUT_PULLUP = 2, LOW = 0;
void pinMode(int, int);
void digitalWrite(int, int);
void delay(unsigned);
unsigned long millis();
''')
            (tmp / "driver").mkdir()
            (tmp / "driver/gpio.h").write_text(r'''
#pragma once
constexpr int GPIO_NUM_44 = 44;
void gpio_hold_dis(int);
''')
            (tmp / "esp_rom_sys.h").write_text("#pragma once\nint esp_rom_printf(const char*, ...);\n")
            (tmp / "Wire.h").write_text(r'''
#pragma once
#include <array>
#include <cstdint>
#include <vector>
struct MockWire {
  std::array<uint8_t, 256> regs{};
  std::vector<uint8_t> writes;
  std::vector<uint8_t> tx;
  bool present = true;
  int reads = 0, failedRead = -1, writeCount = 0, failedWrite = -1;
  uint8_t currentReg = 0;
  bool begin(int, int, int) { return true; }
  void setTimeOut(int) {}
  void beginTransmission(uint8_t addr) { (void)addr; tx.clear(); }
  void write(uint8_t value) { tx.push_back(value); }
  int endTransmission(bool = true) {
    if (tx.size() == 1) {
      currentReg = tx[0];
      return !present || ++reads == failedRead;
    }
    if (tx.size() != 2 || !present || ++writeCount == failedWrite) return 1;
    regs[tx[0]] = tx[1];
    writes.push_back(tx[0]);
    return 0;
  }
  uint8_t requestFrom(uint8_t, uint8_t count, uint8_t) { return present ? count : 0; }
  uint8_t read() { return regs[currentReg]; }
  int available() { return 0; }
};
inline MockWire Wire;
''')
            (tmp / "check.cpp").write_text(r'''
#include <MetalioEink4Board.h>
#include <Wire.h>
#include <cassert>
#include <vector>

using freeink::metalio::ChargerConfig;
using freeink::metalio::ChargerConfigResult;
constexpr ChargerConfig config{4350, 240, 60, 480, 480};

void reset() {
  Wire = MockWire{};
  Wire.regs.fill(0xff);
  Wire.regs[0x38] = 0xa5;
  freeink::metalio::ready = true;
}

int main() {
  uint8_t partInfo = 0;
  reset();
  freeink::metalio::ready = false;
  assert(freeink::metalio::configureCharger(config, partInfo) == ChargerConfigResult::BusNotReady);
  assert(Wire.reads == 0 && Wire.writes.empty());

  reset();
  const ChargerConfig invalid{4355, 240, 60, 480, 480};
  assert(freeink::metalio::configureCharger(invalid, partInfo) == ChargerConfigResult::InvalidConfig);
  assert(Wire.reads == 0 && Wire.writes.empty());

  reset();
  assert(freeink::metalio::configureCharger(config, partInfo) == ChargerConfigResult::Configured);
  assert(partInfo == 0xa5);
  const std::vector<uint8_t> order = {
    0x16, 0x10, 0x11, 0x12, 0x13, 0x04, 0x05,
    0x02, 0x03, 0x06, 0x07, 0x14, 0x16
  };
  assert(Wire.writes == order);
  assert(Wire.regs[0x10] == 0xcf && Wire.regs[0x11] == 0xfe);
  assert(Wire.regs[0x12] == 0x37 && Wire.regs[0x13] == 0xfe);
  assert(Wire.regs[0x04] == 0x9f && Wire.regs[0x05] == 0xfd);
  assert(Wire.regs[0x02] == 0xbf && Wire.regs[0x03] == 0xf1);
  assert(Wire.regs[0x06] == 0x8f && Wire.regs[0x07] == 0xf1);
  assert(Wire.regs[0x14] == 0xff);  // Hardware termination enabled.
  assert(Wire.regs[0x16] == 0xec);  // Charging on, HIZ/WDT off; unrelated bits kept.

  reset();
  const ChargerConfig highCodes{4800, 400, 330, 3040, 3000};
  assert(freeink::metalio::configureCharger(highCodes, partInfo) == ChargerConfigResult::Configured);
  assert(((Wire.regs[0x10] >> 4) | ((Wire.regs[0x11] & 1) << 4)) == 20);
  assert(((Wire.regs[0x12] >> 3) | ((Wire.regs[0x13] & 1) << 5)) == 33);
  assert(((Wire.regs[0x04] >> 3) | ((Wire.regs[0x05] & 15) << 5)) == 480);
  assert(((Wire.regs[0x02] >> 6) | ((Wire.regs[0x03] & 15) << 2)) == 38);
  assert(((Wire.regs[0x06] >> 4) | ((Wire.regs[0x07] & 15) << 4)) == 150);

  reset();
  Wire.present = false;
  assert(freeink::metalio::configureCharger(config, partInfo) == ChargerConfigResult::ProbeFailed);
  assert(Wire.writes.empty());

  for (int read = 2; read <= 14; ++read) {
    reset();
    Wire.failedRead = read;
    assert(freeink::metalio::configureCharger(config, partInfo) == ChargerConfigResult::IoError);
    if (read > 2) assert((Wire.regs[0x16] & 0x20) == 0);
  }

  for (int write = 1; write <= 13; ++write) {
    reset();
    Wire.failedWrite = write;
    assert(freeink::metalio::configureCharger(config, partInfo) == ChargerConfigResult::IoError);
    assert(Wire.writes.size() == static_cast<size_t>(write - 1));
    if (write > 1) assert((Wire.regs[0x16] & 0x20) == 0);
  }
}
''')
            subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(tmp),
                 "-I", str(BOARD_INCLUDE), str(tmp / "check.cpp"),
                 "-o", str(tmp / "check")],
                check=True,
            )
            subprocess.run([str(tmp / "check")], check=True)


if __name__ == "__main__":
    unittest.main()
