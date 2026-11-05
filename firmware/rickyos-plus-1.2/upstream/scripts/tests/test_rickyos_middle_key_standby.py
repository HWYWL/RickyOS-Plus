"""Run the 1.2 product's production Back-release branch and independent power path."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class RickyMiddleKeyStandbyTest(unittest.TestCase):
    def test_back_release_respects_the_same_switch_as_home_entry(self):
        source = (ROOT / 'src/activities/apps/standby/StandbyActivity.cpp').read_text()
        loop = method(source, 'void StandbyActivity::loop()')
        back = method(loop, 'if (mappedInput.wasReleased(MappedInputManager::Button::Back))')
        program = r'''
#include <cassert>
#define STANDBY_DIAG(...) ((void)0)
struct MappedInputManager {
  enum class Button {Back};
  bool released=false;
  bool wasReleased(Button) const {return released;}
};
struct {bool standbyShortcutEnabled=false;} SETTINGS;
struct {int homes=0;void goHome(){++homes;}} activityManager;
struct StandbyActivity {
  MappedInputManager mappedInput;
  int exits=0,continued=0;
  void finish(){++exits;}
  void loop();
};
void StandbyActivity::loop() {
''' + back + r'''
  ++continued;
}
int main() {
  StandbyActivity standby;
  standby.loop();
  assert(standby.continued==1 && standby.exits==0 && activityManager.homes==0);
  standby.mappedInput.released=true;
  for(int attempt=0;attempt<5;++attempt) standby.loop();
  assert(standby.exits==0 && activityManager.homes==0 && standby.continued==1);
  SETTINGS.standbyShortcutEnabled=true;
  standby.loop();
  assert(standby.exits==1 && activityManager.homes==0 && standby.continued==1);
  SETTINGS.standbyShortcutEnabled=false;
  standby.loop();
  assert(standby.exits==1);
  standby.mappedInput.released=false;
  standby.loop();
  assert(standby.continued==2);
}
'''
        run_cpp(program, defines=('RICKYOS_PRODUCT',))
        entry = method((ROOT / 'src/activities/ActivityManager.cpp').read_text(),
                       'bool ActivityManager::handleHomeStandbyInput()')
        self.assertIn('!SETTINGS.standbyShortcutEnabled', entry)

    def test_power_key_and_timer_paths_remain_independent(self):
        main = (ROOT / 'src/main.cpp').read_text()
        side = main[main.index('// Side key: a short press toggles Standby'):
                    main.index('if (!kRickyPowerModel')]
        self.assertNotIn('standbyShortcutEnabled', side)
        self.assertIn('activityManager.closeStandby()', side)
        self.assertIn('activityManager.openStandby()', side)
        self.assertIn('enterDeepSleep()', side)
        standby = (ROOT / 'src/activities/apps/standby/StandbyActivity.cpp').read_text()
        light_sleep = method(standby, 'bool StandbyActivity::tryLightSleep(')
        self.assertNotIn('standbyShortcutEnabled', light_sleep)
        self.assertIn('activityManager.closeStandby()', light_sleep)


if __name__ == '__main__':
    unittest.main()
