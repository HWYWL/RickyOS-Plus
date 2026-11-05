"""RickyOS side key: short press toggles Standby, a hold powers off; idle opens Standby."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method

ROOT = Path(__file__).resolve().parents[2]


def read(rel):
    return (ROOT / rel).read_text()


class RickyPowerModelTest(unittest.TestCase):
    def test_side_key_short_press_toggles_standby_and_hold_powers_off(self):
        main = read('src/main.cpp')
        self.assertIn('constexpr unsigned long RICKY_POWER_OFF_HOLD_MS = 2000;', main)
        block = main[main.index('// Side key: a short press toggles Standby'):]
        block = block[:block.index('#endif')]
        self.assertIn('now - pressedAt >= RICKY_POWER_OFF_HOLD_MS', block)
        self.assertLess(block.index('enterDeepSleep();'), block.index('activityManager.openStandby();'))
        self.assertIn('activityManager.closeStandby();', block)
        # A failed PMU poll reads as released: only a release lasting 200 ms counts, so a
        # hold is never split into short presses (measured on the device: it toggled Standby).
        self.assertIn('constexpr unsigned long kSideKeyReleaseMs = 200;', block)
        self.assertIn('now - lastDownAt >= kSideKeyReleaseMs', block)
        # The press that powered the device on never toggles Standby or powers off.
        self.assertIn('wakePressPending', block)
        self.assertIn('!wakePressPending && !holdHandled', block)
        # The generic hold-to-sleep path is off for RickyOS.
        self.assertIn('if (!kRickyPowerModel && !x4ProAwaitingClickWindow', main)

    def test_idle_opens_standby_and_standby_powers_off_after_its_time(self):
        main = read('src/main.cpp')
        idle = main[main.index('// RickyOS power model:'):]
        idle = idle[:idle.index('static bool powerReleasedSinceWake')]
        self.assertIn('activityManager.openStandby();', idle)
        self.assertNotIn('enterDeepSleep(true);\n    // This', idle)
        self.assertLess(idle.index('consumePowerOffRequest()'), idle.index('openStandby()'))
        standby = read('src/activities/apps/standby/StandbyActivity.cpp')
        self.assertIn('activityManager.requestPowerOff();', standby)
        self.assertIn('millis() - enteredMs_ >= powerOffMs', standby)
        self.assertIn('READING_STATS.resumeSession();', method(standby, 'void StandbyActivity::onExit()'))
        settings = read('src/CrossPointSettings.h')
        self.assertIn('static constexpr uint8_t RICKY_AUTO_OFF_HOURS[] = {0, 1, 3, 6, 12, 24};', settings)
        self.assertIn('uint8_t rickyAutoOffIndex = 3;  // 6 hours', settings)
        manager = read('src/activities/ActivityManager.cpp')
        open_standby = method(manager, 'void ActivityManager::openStandby()')
        self.assertIn('pushActivity(std::move(standby));', open_standby)
        self.assertIn('popActivity();', method(manager, 'void ActivityManager::closeStandby()'))

    def test_short_press_has_no_other_action(self):
        load = read('src/CrossPointSettings.cpp')
        load = load[load.index('enforceProductLayout();\n'):]
        self.assertIn('shortPwrBtn = IGNORE;', load[:400])
        # When Standby starts and when it powers off sit on the Standby & power-off page.
        page = read('src/activities/apps/standby/RickyStandbySettingsActivity.cpp')
        self.assertIn('SETTINGS.sleepTimeoutMinutes = kStandbyMinutes[chosen];', page)
        self.assertIn('SETTINGS.rickyAutoOffIndex = static_cast<uint8_t>(chosen);', page)
        settings = read('src/activities/settings/SettingsActivity.cpp')
        hidden = settings[settings.index('const auto elsewhere'):]
        self.assertIn('&CrossPointSettings::sleepTimeoutMinutes', hidden[:hidden.index('for (auto* list')])

    def test_side_key_wakes_standby_light_sleep(self):
        # The PMU holds its interrupt while key events are queued; unacknowledged, a
        # full FIFO meant no press could end Standby's light sleep (device: only a
        # hold worked once Standby had slept). Drain before and after every sleep.
        import sys
        sys.path.insert(0, str(ROOT / 'scripts'))
        import patch_rickyos_pmu_keys as patch
        self.assertIn('pre:scripts/patch_rickyos_pmu_keys.py', read('platformio.ini'))
        board = read('freeink-sdk/libs/hardware/BoardReadPico/src/BoardReadPico.cpp')
        header = read('freeink-sdk/libs/hardware/BoardReadPico/include/BoardReadPico.h')
        for source, old, new, previous in ((header, patch.OLD_DECL, patch.NEW_DECL, (patch.V2_DECL, patch.V1_DECL)),
                                           (board, patch.OLD_DEF, patch.NEW_DEF, (patch.V2_DEF, patch.V1_DEF))):
            patched = patch.patch_text(source, old, new, previous)
            self.assertIn(new, patched)
            self.assertEqual(patched, patch.patch_text(patched, old, new, previous))
        self.assertIn('if (!pmuEventAck(id)) break;', patch.NEW_DEF)
        hal = read('lib/hal/HalPowerManager.cpp')
        sleep = hal[hal.index('HalPowerManager::LightSleepWakeReason lightSleepReadPico('):]
        self.assertLess(sleep.index('BoardReadPico::takePmuKeyPress(keyDown, boundary);'), sleep.index('esp_light_sleep_start()'))
        self.assertLess(sleep.index('esp_light_sleep_start()'),
                        sleep.index('BoardReadPico::takePmuKeyPress(keyDown, lastIdBeforeSleep)'))
        standby = method(read('src/activities/apps/standby/StandbyActivity.cpp'),
                         'bool StandbyActivity::tryLightSleep(')
        quick = standby[standby.index('LightSleepWakeReason::PowerButton:'):standby.index('LightSleepWakeReason::PowerButtonHeld:')]
        self.assertIn('activityManager.closeStandby();', quick)
        main = read('src/main.cpp')
        self.assertIn('if (standbyWasShowing && !standbyShowingNow) lastActivityTime = millis();', main)
    def test_old_key_event_cannot_close_standby(self):
        # Device, 1.1.3: a side-key press left its event queued while STATUS said no
        # events were pending, so the pre-sleep drain skipped it; read after the next
        # auto-Standby light sleep, it looked like a press and closed Standby by itself.
        import sys
        sys.path.insert(0, str(ROOT / 'scripts'))
        import patch_rickyos_pmu_keys as patch
        drain = patch.NEW_DEF[patch.NEW_DEF.index('bool takePmuKeyPress('):patch.NEW_DEF.index('uint16_t pmuLastEventId() {')]
        # Read until EVENT_PEEK says empty instead of trusting the pending count.
        self.assertNotIn('g_pmuPendingEvents', drain)
        self.assertIn('if (!pmuPeekEvent(id, type)) break;', drain)
        # Only an event newer than the pre-sleep boundary counts as a press.
        self.assertIn('static_cast<int16_t>(id - newerThan) > 0', drain)
        self.assertIn('if (fresh && (type == kKeyDown || type == kKeyShort)) pressed = true;', drain)
        self.assertIn('uint16_t newerThan = 0', patch.NEW_DECL)
        hal = read('lib/hal/HalPowerManager.cpp')
        sleep = hal[hal.index('HalPowerManager::LightSleepWakeReason lightSleepReadPico('):]
        boundary = sleep.index('const uint16_t lastIdBeforeSleep = BoardReadPico::pmuLastEventId();')
        self.assertLess(sleep.index('BoardReadPico::takePmuKeyPress(keyDown, boundary);'), boundary)
        self.assertLess(boundary, sleep.index('esp_light_sleep_start()'))
        # A tree patched by 1.1.3 upgrades in place instead of failing the build.
        for earlier in (patch.V1_DEF, patch.V2_DEF):
            upgraded = patch.patch_text('x\n' + earlier + '\ny', patch.OLD_DEF, patch.NEW_DEF, (patch.V2_DEF, patch.V1_DEF))
            self.assertIn(patch.NEW_DEF, upgraded)
            self.assertNotIn(earlier, upgraded)

    def test_quick_tap_that_wakes_standby_leaves_it(self):
        # Device: a tap that ended the light sleep was sometimes over before
        # main.cpp's key poll saw it held, so Standby stayed and a second press was
        # needed. The wake path now watches the key briefly and reports a tap itself.
        hal = read('lib/hal/HalPowerManager.cpp')
        sleep = hal[hal.index('HalPowerManager::LightSleepWakeReason lightSleepReadPico('):]
        watch = sleep[sleep.index('if (wakePress && keyDown) {'):sleep.index('if (wakePress) {')]
        self.assertIn('constexpr unsigned long kTapWindowMs = 500;', watch)
        self.assertIn('takePmuKeyPress(keyDown, BoardReadPico::pmuLastEventId())', watch)
        self.assertIn('return keyDown ? LightSleepWakeReason::PowerButtonHeld : LightSleepWakeReason::PowerButton;',
                      sleep[sleep.index('if (wakePress) {'):])

    def test_press_during_clock_redraw_is_not_dropped(self):
        # Device: the minute clock redraw holds the main loop ~3 s; a tap then was
        # drained before the next sleep and lost. Presses newer than the previous
        # wake are answered instead; a Standby that is just opening forgets older ones.
        hal = read('lib/hal/HalPowerManager.cpp')
        sleep = hal[hal.index('HalPowerManager::LightSleepWakeReason lightSleepReadPico('):]
        pre = sleep[:sleep.index('esp_light_sleep_start()')]
        self.assertIn('if (boundary != 0 && (queuedPress || keyDown)) {', pre)
        self.assertIn('g_standbyKeyBoundary = BoardReadPico::pmuLastEventId();', sleep)
        self.assertIn('HalPowerManager::beginStandbyKeyWatch();',
                      method(read('src/activities/apps/standby/StandbyActivity.cpp'), 'void StandbyActivity::onEnter('))
        # No light sleep while main.cpp is still judging a side-key press.
        self.assertIn('!g_sideKeyBusy.load(std::memory_order_relaxed)', hal)
        self.assertIn('HalPowerManager::setSideKeyBusy(keyDown);', read('src/main.cpp'))

    def test_standby_ignores_edge_swipes(self):
        manager = read('src/activities/ActivityManager.cpp')
        self.assertIn('const bool atRest = standbyShowing();', manager)
        self.assertIn('if (!atRest && !currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {', manager)
        self.assertIn('if (!atRest && currentActivity->name != "FrontlightPanel"', manager)

if __name__ == '__main__':
    unittest.main()
