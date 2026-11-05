"""Validate the maintained simulator adapter, including real timed waits."""

import importlib.util
import tempfile
import unittest
from pathlib import Path

from test_reading_ui_regressions import run_cpp

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('sim_notify_patch', ROOT / 'scripts/patch_rickyos_simulator.py')
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


class SimulatorNotificationsTest(unittest.TestCase):
    def test_exact_patch_is_idempotent_and_rejects_unknown_input(self):
        with tempfile.TemporaryDirectory(prefix='ricky-sim-notify-') as directory:
            path = Path(directory) / 'task.h'
            path.write_text(patch.OLD_NOTIFY)
            self.assertTrue(patch.patch_task_notify(path))
            self.assertEqual(path.read_text(), patch.NEW_NOTIFY)
            self.assertFalse(patch.patch_task_notify(path))
            path.write_text('unknown upstream shim')
            with self.assertRaises(RuntimeError):
                patch.patch_task_notify(path)
            self.assertEqual(path.read_text(), 'unknown upstream shim')

    def test_zero_finite_infinite_waits_and_both_counter_modes(self):
        run_cpp(r'''
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#define portMAX_DELAY 0xFFFFFFFF
#define portTICK_PERIOD_MS 1
struct Task {
  std::mutex mtx;
  std::condition_variable cv;
  uint32_t notifyCount = 0;
} task;
Task* xTaskGetCurrentTaskHandle() { return &task; }
''' + patch.NEW_NOTIFY + r'''
int main() {
  assert(ulTaskNotifyTake(1, 0) == 0);
  assert(ulTaskNotifyTake(1, 5) == 0);
  task.notifyCount = 3;
  assert(ulTaskNotifyTake(0, 0) == 3 && task.notifyCount == 2);
  assert(ulTaskNotifyTake(1, 0) == 2 && task.notifyCount == 0);
  for (const uint32_t wait : {1000u, uint32_t(portMAX_DELAY)}) {
    std::thread publisher([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      { std::lock_guard<std::mutex> lock(task.mtx); task.notifyCount = 2; }
      task.cv.notify_one();
    });
    assert(ulTaskNotifyTake(1, wait) == 2);
    publisher.join();
    assert(task.notifyCount == 0);
  }
}
''')

    def test_adapter_is_scoped_to_the_product_simulator(self):
        source = (ROOT / 'scripts/patch_rickyos_simulator.py').read_text()
        self.assertIn("env.subst('$PIOENV') == 'simulator_rickyos'", source)
        config = (ROOT / 'platformio.ini').read_text()
        product = config.split('[env:simulator_rickyos]', 1)[1].split('\n[', 1)[0]
        self.assertIn('post:scripts/patch_rickyos_simulator.py', product)
        hardware = config.split('[env:rickyos_readpico]', 1)[1].split('\n[', 1)[0]
        self.assertNotIn('patch_rickyos_simulator.py', hardware)


if __name__ == '__main__':
    unittest.main()
