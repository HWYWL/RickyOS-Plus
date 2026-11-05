"""Run the real RTC driver against a recording I2C bus (no device required)."""
from pathlib import Path
import subprocess
import tempfile

RTC = Path(__file__).resolve().parents[2]

with tempfile.TemporaryDirectory(prefix="freeink-rtc-test-") as directory:
    root = Path(directory)
    (root / "soc").mkdir()
    (root / "soc/soc_caps.h").write_text("#define SOC_I2C_NUM 2\n")
    (root / "Arduino.h").write_text("#pragma once\n#include <cstdint>\n")
    (root / "BoardConfig.h").write_text("""
#pragma once
#include <cstdint>
#define FREEINK_CAP_RTC 1
#define FREEINK_DEVICE_MURPHY_M4 0
namespace BoardConfig {
enum class RtcType { None, Pcf8563, Pcf85063, Ds3231, Rx8130, Rx8010 };
struct Sensors {
  uint8_t rtcAddr = 0x51;
  int i2cSda = 41, i2cScl = 42, i2cHz = 400000, i2cBus = 0;
  RtcType rtcType = RtcType::Pcf85063;
};
inline struct { Sensors sensors; } ACTIVE;
}
""")
    (root / "Wire.h").write_text("""
#pragma once
#include <cstdint>
struct TwoWire {
  uint8_t regs[256] = {}, cursor = 0, firstRegister = 0;
  bool addressNext = true, fail = false;
  unsigned transactions = 0;
  void begin(int, int, int) {}
  void beginTransmission(uint8_t) { addressNext = true; ++transactions; }
  void write(uint8_t value) {
    if (addressNext) { cursor = firstRegister = value; addressNext = false; }
    else { regs[cursor++] = value; }
  }
  int endTransmission(bool = true) { return fail ? 1 : 0; }
  uint8_t requestFrom(uint8_t, uint8_t count, uint8_t) { return fail ? 0 : count; }
  uint8_t read() { return regs[cursor++]; }
};
inline TwoWire Wire, Wire1;
""")
    (root / "test.cpp").write_text("""
#include <cassert>
#include <initializer_list>
#include <Rtc.h>
#include <Wire.h>
int main() {
  Rtc rtc;
  assert(rtc.begin() && Wire.firstRegister == 0x00);
  Rtc::DateTime date{2026, 9, 20, 13, 42, 15, 0}, readback;
  assert(rtc.set(date) && Wire.firstRegister == 0x04);
  assert(rtc.now(readback));
  assert(readback.year == 2026 && readback.month == 9 && readback.day == 20);
  assert(readback.hour == 13 && readback.minute == 42 && readback.second == 15);
  Wire.regs[0x04] |= 0x80;
  assert(!rtc.now(readback));
  assert(rtc.set(date) && !(Wire.regs[0x04] & 0x80));
  for (uint16_t year : {2000, 2099}) {
    date.year = year;
    assert(rtc.set(date) && rtc.now(readback) && readback.year == year);
  }
  for (uint16_t year : {1999, 2100}) {
    date.year = year;
    const auto before = Wire.transactions;
    assert(!rtc.set(date) && Wire.transactions == before);
  }
  date.year = 2026;
  Wire.fail = true;
  Rtc unavailable;
  assert(!unavailable.begin() && !rtc.now(readback) && !rtc.set(date));
}
""")
    binary = root / "rtc-test"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(root), "-I", str(RTC / "include"),
                    str(RTC / "src/Rtc.cpp"), str(root / "test.cpp"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    print("PCF85063: probe, roundtrip, oscillator flag, year limits and I2C errors passed")
