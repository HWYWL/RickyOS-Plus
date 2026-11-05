"""SC7A20H back double tap: register setup, latch polling, sleep/wake rate, INT drive.

Compiles the real freeink Imu.cpp against the SDK's own Wire stub (the Read Pico
profile), next to its stale test_sc7a20h.cpp, which no longer builds."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
IMU = ROOT / 'freeink-sdk/libs/hardware/Imu'
PROGRAM = r'''
#include <BoardConfig.h>
#include <Imu.h>
#include <Wire.h>
#include <cassert>
int main() {
  Imu imu;
  Wire.devices[0x19] = true;
  Wire.regs[0x19][0x0F] = 0x11;
  Wire.regs[0x19][0x70] = 0x28;
  assert(!imu.enableDoubleTap(1) && !imu.takeDoubleTap());  // not begun
  assert(imu.begin());
  assert(Wire.regs[0x19][0x20] == 0x2F && Wire.regs[0x19][0x25] == 0);  // Pico: INT left alone
  Wire.regs[0x19][0x39] = 0x60;  // a latch from before is read away on enable
  assert(imu.enableDoubleTap(1));
  auto& r = Wire.regs[0x19];
  assert(r[0x3A] == (0x80 | 0x1C) && r[0x3B] == 16 && r[0x3C] == 32 && r[0x3D] == 128);
  assert(r[0x38] == 0x3F && r[0x20] == 0x67);  // click detector on, 200 Hz
  assert(r[0x21] == 0x04);  // the detector sees high-pass data, not gravity
  assert(r[0x24] == 0x40 && r[0x2E] == 0x80);  // FIFO in stream mode for the software detector
  // FIFO read: count from FIFO_SRC, Z from bytes 4..5 of each sample, >> 4, 1 mg/LSB.
  r[0x2F] = 0x40 | 3;
  r[0x2C] = 0x00; r[0x2D] = 0x40;  // z = 0x4000 >> 4 = 1024 mg
  float zs[8] = {};
  bool lost = false;
  assert(imu.readTapAxis(zs, 8, lost) == 3 && lost && zs[0] > 1.0f && zs[0] < 1.05f);  // the stub is no FIFO: later samples read other registers
  r[0x2F] = 31;
  assert(imu.readTapAxis(zs, 8, lost) == 8 && !lost);  // never more than asked
  uint8_t regs[8];
  assert(imu.readTapRegisters(regs) && regs[0] == 0x67 && regs[3] == 0x80 && regs[4] == 0x3F);
  r[0x39] = 0x60;
  assert(imu.takeDoubleTap());
  r[0x39] = 0x50;  // a single click is not a double tap
  assert(!imu.takeDoubleTap());
  r[0x39] = 0x20;  // DCLICK without IA
  assert(!imu.takeDoubleTap());
  assert(imu.sleep() && r[0x20] == 0x00);
  assert(imu.wake() && r[0x20] == 0x67 && r[0x21] == 0x04);  // wake keeps tap rate and filter
  uint8_t src = 0;
  r[0x39] = 0x50;
  assert(!imu.takeDoubleTap(&src) && src == 0x50);  // the raw source is reported for tuning
  assert(imu.disableDoubleTap() && r[0x38] == 0 && r[0x20] == 0x2F && r[0x21] == 0 && r[0x24] == 0 &&
         r[0x2E] == 0);
  assert(imu.readTapAxis(zs, 8, lost) == 0);  // off: no samples
  r[0x39] = 0x60;
  assert(!imu.takeDoubleTap());  // off: never reported
  assert(imu.wake() && r[0x20] == 0x2F);
  assert(imu.disableDoubleTap());  // already off
  assert(imu.enableDoubleTap(0) && r[0x3A] == (0x80 | 0x28));
  assert(imu.enableDoubleTap(2) && r[0x3A] == (0x80 | 0x12));
  assert(imu.enableDoubleTap(9) && r[0x3A] == (0x80 | 0x1C));  // out of range: medium
  Wire.failWriteReg = 0x38;
  assert(!imu.enableDoubleTap(1) && !imu.takeDoubleTap());
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='imu-tap-') as d:
    p = Path(d)
    (p / 'test.cpp').write_text(PROGRAM)
    stubs = IMU / 'test/host'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-Wno-unused-variable', '-Wno-unused-function', '-Wno-unused-const-variable',
                    '-DFREEINK_DEVICE_READPICO=1', '-I', str(stubs / 'stubs'),
                    '-I', str(ROOT / 'freeink-sdk/libs/hardware/InputManager/test/host/metalio_stubs'),
                    '-I', str(ROOT / 'freeink-sdk/libs/hardware/BoardConfig/include'), '-I', str(IMU / 'include'),
                    str(p / 'test.cpp'), str(IMU / 'src/Imu.cpp'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
print('Back double tap: click registers, latch read, single/IA filtering, sleep/wake rate, '
      'disable restores 12.5 Hz, Pico INT untouched')
