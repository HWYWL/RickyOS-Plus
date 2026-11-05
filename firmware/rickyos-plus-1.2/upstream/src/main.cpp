#include <Arduino.h>
#include <BoardConfig.h>
#if FREEINK_DEVICE_METALIO_PLUS && RICKYOS_PLUS_FIELD_DIAG
#include <PlusFieldDiagnostics.h>
#endif
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <BatteryMonitor.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalOtaSlot.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <BatteryMonitor.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SPI.h>
#include <TrustedTime.h>
#include <VectorFontSupport.h>
#include <WiFi.h>
#include <XteinkDetect.h>
#if FREEINK_CAP_TOUCH
#include <esp_sntp.h>
#endif
#include <builtinFonts/all.h>

#include <cstring>

#include "AchievementsStore.h"
#include "BleInput.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "WifiCredentialStore.h"
#include "util/TimeUtils.h"
#include "activities/Activity.h"
#include "util/TimeUtils.h"
#include "activities/ActivityManager.h"
#include "activities/settings/FontDownloadActivity.h"
#include "activities/settings/LanguageSelectActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "activities/settings/TextSettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "platform/UsbSerialJtagHandoff.h"
#include "util/ButtonNavigator.h"
#include "util/RickyStorageLayout.h"
#include "util/ScreenshotUtil.h"
#include "util/Timezones.h"
#include "util/UserGuide.h"
#include "util/BootPreparation.h"

#if CROSSPOINT_VECTOR_FONTS
// Rendering (incl. FreeType TTF rasterization) runs on the Arduino loop task.
// The default 8 KB stack overflows inside FreeType's FT_Open_Face / variable-font
// parsing. This runtime override applies even with the prebuilt (dio_opi) core,
// where CONFIG_ARDUINO_LOOP_STACK_SIZE from sdkconfig is baked in and ignored.
// Vector-font boards only: without TTF the stock loop stack has always sufficed,
// and non-PSRAM boards need the 16KB back in DRAM.
SET_LOOP_TASK_STACK_SIZE(24 * 1024)
#endif

#if CROSSPOINT_CAP_SOUND_FEEDBACK
#include <SoundFeedback.h>
#endif

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
static unsigned long allowSleepAt = 0;
constexpr unsigned long READING_STATS_CHECKPOINT_IDLE_MS = 15UL * 1000UL;
static unsigned long lastX4ProPowerClickAt = 0;

namespace {
constexpr unsigned long X4PRO_POWER_DOUBLE_CLICK_MS = 500;
// Side key held this long powers off; anything shorter toggles Standby.
constexpr unsigned long RICKY_POWER_OFF_HOLD_MS = 2000;
constexpr bool kRickyPowerModel = true;
constexpr unsigned long X4PRO_POWER_CLICK_MAX_HOLD_MS = 300;
#if CROSSPOINT_CAP_SOUND_FEEDBACK
static_assert(CrossPointSettings::SOUND_FEEDBACK_OFF == static_cast<uint8_t>(SoundFeedback::Level::Off));
static_assert(CrossPointSettings::SOUND_FEEDBACK_LOW == static_cast<uint8_t>(SoundFeedback::Level::Low));
static_assert(CrossPointSettings::SOUND_FEEDBACK_MEDIUM == static_cast<uint8_t>(SoundFeedback::Level::Medium));
static_assert(CrossPointSettings::SOUND_FEEDBACK_HIGH == static_cast<uint8_t>(SoundFeedback::Level::High));
static_assert(HalGPIO::BTN_BACK == SoundFeedback::BUTTON_BACK &&
              HalGPIO::BTN_CONFIRM == SoundFeedback::BUTTON_CONFIRM &&
              HalGPIO::BTN_LEFT == SoundFeedback::BUTTON_LEFT && HalGPIO::BTN_RIGHT == SoundFeedback::BUTTON_RIGHT &&
              HalGPIO::BTN_UP == SoundFeedback::BUTTON_UP && HalGPIO::BTN_DOWN == SoundFeedback::BUTTON_DOWN &&
              HalGPIO::BTN_POWER == SoundFeedback::BUTTON_POWER);
#endif

void updateBluetoothLifecycle() {
#if FREEINK_CAP_BLE_HID_HOST
  static unsigned long nextStartAttemptAt = 0;
  const auto wanted = [] {
    return SETTINGS.bluetoothEnabled && activityManager.keepsBluetoothAlive() &&
           !activityManager.requiresExclusiveStorageLoop() && WiFi.getMode() == WIFI_MODE_NULL;
  };
  if (!wanted()) {
    bleinput::stop();
    return;
  }
  // Preparation blocks new starts; C3 and PSRAM readers keep existing links
  // through chapter construction and book indexing.
  if (bleinput::isRunning() || activityManager.deferBluetoothStart() || millis() < nextStartAttemptAt) return;
  // Non-reader pages that keep an existing link alive own their explicit start
  // attempts; only readers use the automatic reader-memory gate and retry loop.
  if (!activityManager.isReaderActivity()) return;
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return;
  // Rendering may have started a chapter build while we were acquiring the lock.
  if (!wanted() || activityManager.deferBluetoothStart() || !activityManager.isReaderActivity() ||
      bleinput::isRunning())
    return;
  const auto result = bleinput::ensureStarted(renderer, bleinput::StartContext::Reader);
  if (result == bleinput::StartResult::LowMemory || result == bleinput::StartResult::Failed) {
    nextStartAttemptAt = millis() + 2000;
  }
#endif
}
}  // namespace

// A wake hold must never become an in-app power-button action.  Boot may continue
// while the button is held; swallow the one release that ends that wake gesture.
static bool wakePowerReleasePending = false;

// Fonts
// All legacy built-in reader IDs share one 12pt offline fallback. Complete
// families, other sizes, and style variants come from SD .cpfont files.
#include <StandbyDiag.h>
#include <builtinFonts/notosans_12_regular.h>
#include <fonts/ricky_caption_10_regular.packed.h>
#include <fonts/ricky_display_23_bold.packed.h>
#include <fonts/ricky_headline_14_medium.packed.h>
#include <fonts/ricky_title_16_bold.packed.h>
#include <fonts/ricky_book_12_medium.packed.h>

#include "components/RickyBadgeNotice.h"
#include "components/RickyDesignUi.h"
// The product's only built-in CJK reading/UI size adds GB2312 level 1 (titles,
// author names); the shared 8/10/12pt set stays as is for other targets.
#include <fonts/notosans_cjk_12_rickyos.packed.h>
static const EpdFontData& cjk12Data = notosans_cjk_12_rickyos;
EpdFont offlineReaderFont(&cjk12Data);
EpdFontFamily offlineReaderFontFamily(&offlineReaderFont);

// Large UI text keeps the upstream face; INX restores its historical fallback
// through the theme reload hook; Ubuntu UI families are shared by all themes.
extern EpdFont ui18RegularFont;
extern EpdFont ui18BoldFont;
EpdFontFamily ui18FontFamily(&ui18RegularFont, &ui18BoldFont);

extern EpdFontFamily control18FontFamily;

// International UI fonts remain primary; CJK subsets are selected only when
// the primary is missing a Han glyph.
EpdFont smallFont(&notosans_12_regular);
EpdFontFamily smallFontFamily(&smallFont);

extern EpdFont ui10RegularFont;
extern EpdFont ui10BoldFont;
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

extern EpdFont ui12RegularFont;
extern EpdFont ui12BoldFont;
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

EpdFontFamily cjk8FontFamily(&offlineReaderFont);
EpdFont cjk14Font(&ricky_headline_14_medium);
EpdFont cjk16Font(&ricky_title_16_bold);
EpdFont designCaptionFont(&ricky_caption_10_regular);
EpdFont designDisplayFont(&ricky_display_23_bold);
EpdFont bookTitleFont(&ricky_book_12_medium);
EpdFontFamily bookTitleFamily(&bookTitleFont);
EpdFontFamily designCaptionFamily(&designCaptionFont);
EpdFontFamily designDisplayFamily(&designDisplayFont);
EpdFontFamily cjk10FontFamily(&designCaptionFont);
EpdFontFamily ui14FallbackFamily(&cjk14Font);
EpdFontFamily ui16FallbackFamily(&cjk16Font);
#ifndef RICKYOS_PRODUCT
// Hebrew/Arabic fallback. The RickyOS product leaves it out to fit the OTA slot (~125 KB):
// its UI is Chinese/English, and an RTL book title draws with the replacement glyph.
static EpdFont rtlRegularFont(&ubuntu_12_regular);
static EpdFont rtlBoldFont(&ubuntu_12_bold);
static EpdFontFamily rtlFontFamily(&rtlRegularFont, &rtlBoldFont);
static constexpr int kRtlFontId = 0x52544C0C;
#endif
EpdFont cjk12Font(&cjk12Data);
EpdFontFamily cjk12FontFamily(&cjk12Font);

// Chinese chess piece glyphs (subset CJK font, 14 characters at 16pt).
EpdFont chineseChessPieceFont(&chinese_chess_16);
EpdFontFamily chineseChessPieceFontFamily(&chineseChessPieceFont);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
RTC_NOINIT_ATTR uint32_t silentRebootFontPointSize;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
enum class SilentRebootTarget : uint32_t {
  Home,
  Reader,
  ReaderSuppressFontPrompt,
  ReaderPreloadChineseFont,
  Settings,
  JoinNetwork,
  Count,
};
RTC_NOINIT_ATTR uint32_t silentRebootPayload;
constexpr uint32_t SILENT_REBOOT_LIGHT_ON = 1U << 0;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,          // cold boot, flash, panic, or plain reboot
  Silent,          // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  SplashlessWake,  // wake from deep sleep with the splash suppressed by the SD flag
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

#if FREEINK_CAP_TOUCH
#if FREEINK_DEVICE_READPICO
// Defer font I/O until after activity teardown releases its render lock.
static bool sdFontReloadPending = false;
#endif

static bool finishWifiSessionWithoutRestart() {
  if (!BoardConfig::hasTouch()) return false;
  if (esp_sntp_enabled()) esp_sntp_stop();
  WiFi.mode(WIFI_OFF);
  delay(100);
  LOG_DBG("MAIN", "WiFi stopped without restart on touch device");
#if FREEINK_DEVICE_READPICO
  sdFontReloadPending = true;
#endif
  return true;
}
#endif

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
#if FREEINK_CAP_TOUCH
  if (finishWifiSessionWithoutRestart()) return;
#endif
  silentRebootTarget = static_cast<uint32_t>(SilentRebootTarget::Home);
  silentRebootFontPointSize = 0;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

// Returns instead of rebooting when sleep supersedes the reboot; callers keep
// running in that case.

void silentRestartToReader(const bool suppressChineseFontPrompt) {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = static_cast<uint32_t>(suppressChineseFontPrompt ? SilentRebootTarget::ReaderSuppressFontPrompt
                                                                       : SilentRebootTarget::Reader);
  silentRebootFontPointSize = 0;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader%s)", suppressChineseFontPrompt ? ", suppress-font-prompt" : "");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReaderAndPreloadChineseFont(const uint8_t pointSize) {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = static_cast<uint32_t>(SilentRebootTarget::ReaderPreloadChineseFont);
  silentRebootFontPointSize = pointSize;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader-preload-font, size=%u)", static_cast<unsigned>(pointSize));
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToSettings() {
  if (deepSleepInProgress) return;
  silentRebootTarget = static_cast<uint32_t>(SilentRebootTarget::Settings);
  silentRebootFontPointSize = 0;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToJoinNetwork() {
  if (deepSleepInProgress) return;
#if FREEINK_CAP_TOUCH
  // A software reset would cycle touch/frontlight rails; those boards proceed
  // into Join Network without the fresh-heap reboot (return, don't stop WiFi —
  // this runs on the way *in*, unlike the exit-time silentRestart()).
  if (BoardConfig::hasTouch()) return;
#endif
  silentRebootTarget = static_cast<uint32_t>(SilentRebootTarget::JoinNetwork);
  silentRebootFontPointSize = 0;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=join-network)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void restartToHomeAfterStorageHandoff() {
  if (deepSleepInProgress) return;  // sleeping supersedes the storage handoff reboot
  silentRebootTarget = static_cast<uint32_t>(SilentRebootTarget::Home);
  silentRebootFontPointSize = 0;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  const unsigned long started = millis();
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  LOG_INF("MAIN", "storage handoff popup %lu ms", millis() - started);
  delay(50);
  handoffUsbOtgToSerialJtag();
  LOG_INF("MAIN", "storage handoff USB switch %lu ms, restarting", millis() - started);
  ESP.restart();
}

void toggleFrontlight() {
  // Global shortcuts run before the active Activity gets its input. Standby
  // must not change the light or persist a preference from an ignored gesture.
  if (activityManager.standbyShowing() || !Frontlight.present()) return;
  const bool lightOn = !Frontlight.isOn();
  Frontlight.setOn(lightOn);
  SETTINGS.frontlightOn = lightOn ? 1 : 0;
  SETTINGS.saveToFile();
  LOG_INF("LIGHT", "Frontlight toggled %s", lightOn ? "on" : "off");
}

bool handleX4ProFrontlightDoubleClick() {
  if (!BoardConfig::isX4Pro() || !SETTINGS.doubleClickPwrLight || !gpio.wasReleased(HalGPIO::BTN_POWER)) {
    return false;
  }

  const unsigned long now = millis();
  if (gpio.getPowerButtonHeldTime() > X4PRO_POWER_CLICK_MAX_HOLD_MS) {
    lastX4ProPowerClickAt = 0;
    return false;
  }
  if (lastX4ProPowerClickAt == 0 || now - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = now;
    return false;
  }

  lastX4ProPowerClickAt = 0;
  toggleFrontlight();
  return true;
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
#if RICKYOS_PLUS_POWER_LOG
// Power measurement (diagnostic builds only). One CSV row per event: wall clock (0 until
// the clock is valid), uptime, event, current activity, USB / frontlight / Wi-Fi state and
// the BQ27220 readings. Usage per state comes from the raw coulomb counter's change over
// time; the "off" row before shutdown and the next "boot" row bracket the powered-off span.
char g_powerLogActivity[24] = "-";
void powerLog(const char* event) {
  static const BatteryMonitor gauge;
  const auto s = gauge.readPowerSnapshot();
  // Ticks also carry the gauge's own StateOfCharge and the percentage on screen.
  char tagged[64];
  if (strncmp(event, "tick", 4) == 0) {
    snprintf(tagged, sizeof(tagged), "%s-soc%u-ui%u", event, s.socOk ? s.soc : 999u,
             static_cast<unsigned>(powerManager.getBatteryPercentage()));
    event = tagged;
  }
  char line[192];
  const int n = snprintf(line, sizeof(line), "%lu,%lu,%s,%s,%d,%d,%d,%u,%d,%d,%u,%d,%d%d%d%d%d\n",
                         static_cast<unsigned long>(TimeUtils::getCurrentValidTimestamp()),
                         static_cast<unsigned long>(millis()), event, g_powerLogActivity, gpio.isUsbConnected() ? 1 : 0,
                         Frontlight.isOn() ? 1 : 0, static_cast<int>(WiFi.getMode()), s.mv, s.current, s.average,
                         s.counter, s.deciCelsius, s.mvOk, s.currentOk, s.averageOk, s.counterOk, s.temperatureOk);
  if (n <= 0) return;
  constexpr const char* kPath = "/rickyos-power.csv";
  const bool fresh = !Storage.exists(kPath);
  HalFile file = Storage.open(kPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) return;
  if (fresh) {
    static const char kHeader[] =
        "epoch,uptime_ms,event,activity,usb,frontlight,wifi,mv,current_ma,average_ma,counter_mah,temp_dc,ok\n";
    file.write(reinterpret_cast<const uint8_t*>(kHeader), sizeof(kHeader) - 1);
  }
  file.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(std::min(n, int(sizeof(line) - 1))));
}
#endif

void enterDeepSleep(bool fromTimeout = false) {
#if RICKYOS_PLUS_POWER_LOG
  powerLog(fromTimeout ? "off-timeout" : "off");
#endif
  bleinput::stop();
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  // Sleep may end in a power-off (battery death, latch); persist the clock
  // floor now so a later cold boot resumes from it.
  trustedtime::note();

  // One source of truth: the Standby & power-off page. The separate "keep the page on
  // automatic sleep" switch could stay on after the mode changed and override it.
  const bool isQuickResumeSleep =
      SETTINGS.rickySleepScreenMode() == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME;
  // Activity-owned state (reading progress, session) is saved before the sleep screen.
  activityManager.prepareForSleep();
  // Every sleep mode leaves a complete retained frame on the e-ink panel. Keep
  // it visible until the first useful reader or home paint replaces it.
  APP_STATE.showBootScreen = false;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (!READING_STATS.saveToFile()) {
    LOG_ERR("RST", "Failed to save reading stats before deep sleep");
  }
  if (!ACHIEVEMENTS.saveToFile()) {
    LOG_ERR("ACH", "Failed to save achievements before deep sleep");
  }

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  } else if (Storage.exists(SLEEP_FRAME_FILE)) {
    // A stale Quick Resume frame must not replace the selected sleep screen during wake.
    Storage.remove(SLEEP_FRAME_FILE);
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

#if FREEINK_CAP_HAPTIC || FREEINK_DEVICE_METALIO_PLUS
  gpio.stopHapticFeedback();
#endif
  halTiltSensor.deepSleep();
  Frontlight.setOn(false);
#if CROSSPOINT_CAP_SOUND_FEEDBACK
  SoundFeedback::shutdown();
#endif
  display.deepSleep();
  Storage.prepareForDeepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

bool setupDisplayAndFonts(bool seamless = false, bool logSdFontLoadHeap = false) {
  display.begin(seamless);
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  // Read Pico step 4 of 5 (see the boot-order note in setup()): the panel has
  // stepped through its own rails by now — BoardReadPico::epdPrepare/On/Off are
  // the board's LgfxEpdPowerHooks, so the SY7636A enable (FCA9555 P0.3), VCOM and
  // the PGOOD wait all ran inside display.begin(). Only now does the CST836U
  // probe go out on the shared 400 kHz bus, which is what HalGPIO::begin()
  // deferred. Idempotent, so any other path that initialises the panel is safe.
  gpio.beginInput();
#endif
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  const bool fontDecompressorReady = fontDecompressor.init();
  if (!fontDecompressorReady) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, offlineReaderFontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, offlineReaderFontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, offlineReaderFontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, offlineReaderFontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, offlineReaderFontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, offlineReaderFontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, offlineReaderFontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, ui18FontFamily);
#endif  // OMIT_FONTS
  // The fixed slider title font keeps its face while INX uses its historical large-text fallback.
  renderer.insertFont(CONTROL_18_FONT_ID, control18FontFamily);
  renderer.insertFont(UI_10_FONT_ID, cjk12FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui16FallbackFamily);
  renderer.insertFont(SMALL_FONT_ID, designCaptionFamily);
  renderer.insertFont(CJK_UI_8_FONT_ID, cjk8FontFamily);
  renderer.insertFont(CJK_UI_10_FONT_ID, cjk10FontFamily);
  renderer.insertFont(CJK_UI_12_FONT_ID, cjk12FontFamily);
  renderer.setFallbackFont(SMALL_FONT_ID, CJK_UI_8_FONT_ID);
  renderer.setFallbackFont(CONTROL_18_FONT_ID, CJK_UI_12_FONT_ID);
  renderer.setFallbackFont(UI_10_FONT_ID, CJK_UI_10_FONT_ID);
  renderer.setFallbackFont(UI_12_FONT_ID, CJK_UI_12_FONT_ID);
  renderer.setFallbackFont(NOTOSANS_18_FONT_ID, CJK_UI_12_FONT_ID);
  renderer.insertFont(BaseTheme::STATUS_NUMERIC_FONT_ID, smallFontFamily);
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  renderer.insertFont(READER_STATUS_FONT_ID, smallFontFamily);
  renderer.insertFont(READER_ESTIMATE_FONT_ID, ui10FontFamily);
  renderer.setFallbackFont(READER_STATUS_FONT_ID, CJK_UI_8_FONT_ID);
  renderer.setFallbackFont(READER_ESTIMATE_FONT_ID, CJK_UI_10_FONT_ID);
#endif
  renderer.insertFont(UiHighDpiProfile::reader12FontId, offlineReaderFontFamily);
  // Same-size Chinese UI first, then common Chinese and Ubuntu script coverage.
  renderer.insertFont(CJK_UI_14_FONT_ID, ui14FallbackFamily);
  renderer.insertFont(CJK_UI_16_FONT_ID, ui16FallbackFamily);
  renderer.setFallbackFont(READER_STATUS_FONT_ID, CJK_UI_12_FONT_ID);
  renderer.setFallbackFont(READER_ESTIMATE_FONT_ID, CJK_UI_14_FONT_ID);
  renderer.setFallbackFont(SMALL_FONT_ID, CJK_UI_12_FONT_ID);
  renderer.setFallbackFont(UI_10_FONT_ID, CJK_UI_14_FONT_ID);
  renderer.setFallbackFont(UI_12_FONT_ID, CJK_UI_16_FONT_ID);
  renderer.setFallbackFont(NOTOSANS_18_FONT_ID, CJK_UI_16_FONT_ID);
  renderer.setFallbackFont(CONTROL_18_FONT_ID, CJK_UI_16_FONT_ID);
#ifndef RICKYOS_PRODUCT
  renderer.insertFont(kRtlFontId, rtlFontFamily);
#endif
  renderer.setFallbackFont(CJK_UI_14_FONT_ID, CJK_UI_12_FONT_ID);
  renderer.setFallbackFont(CJK_UI_16_FONT_ID, CJK_UI_12_FONT_ID);
#ifndef RICKYOS_PRODUCT
  renderer.setFallbackFont(CJK_UI_12_FONT_ID, kRtlFontId);
#endif
  renderer.insertFont(RickyDesignUi::BODY, cjk12FontFamily);
  renderer.insertFont(RickyDesignUi::BOOK_TITLE, bookTitleFamily);
  renderer.insertFont(RickyDesignUi::CAPTION, designCaptionFamily);
  renderer.insertFont(RickyDesignUi::HEADLINE, ui14FallbackFamily);
  renderer.insertFont(RickyDesignUi::TITLE, ui16FallbackFamily);
  renderer.insertFont(RickyDesignUi::NUMERIC_DISPLAY, designDisplayFamily);
#ifndef RICKYOS_PRODUCT
  renderer.setFallbackFont(RickyDesignUi::BODY, kRtlFontId);
#endif
  renderer.setFallbackFont(RickyDesignUi::CAPTION, RickyDesignUi::BODY);
  // Existing functional pages keep their stable IDs, but use the same Noto
  // family as the redesigned pages. Reader families are not rebound.
  renderer.setFallbackFont(SMALL_FONT_ID, RickyDesignUi::BODY);
#ifndef RICKYOS_PRODUCT
  renderer.setFallbackFont(UI_10_FONT_ID, kRtlFontId);
#endif
  renderer.setFallbackFont(UI_12_FONT_ID, RickyDesignUi::BODY);
  renderer.insertFont(CHINESE_CHESS_FONT_ID, chineseChessPieceFontFamily);

  // Discover and load SD card fonts
  if (logSdFontLoadHeap) {
    LOG_INF("FONT", "Clean restart before font load: free=%u, maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
  return fontDecompressorReady;
}

void continueChineseFontInstall(const uint8_t expectedPointSize) {
  if (APP_STATE.openEpubPath.empty() || !Storage.exists(APP_STATE.openEpubPath.c_str())) {
    LOG_ERR("FONT", "Cannot resume automatic font install: original EPUB is unavailable");
    activityManager.goHome();
    return;
  }

  const bool fontReady =
      strcmp(SETTINGS.sdFontFamilyName, SdCardFontSystem::COMPLETE_CHINESE_NOTO_SANS_FAMILY) == 0 &&
      SETTINGS.fontPointSize == expectedPointSize &&
      sdFontSystem.resolveFontId(SdCardFontSystem::COMPLETE_CHINESE_NOTO_SANS_FAMILY, expectedPointSize) != 0;
  if (!fontReady) {
    LOG_ERR("FONT", "Failed to load selected family %s at point size %u after clean restart",
            SdCardFontSystem::COMPLETE_CHINESE_NOTO_SANS_FAMILY, static_cast<unsigned>(expectedPointSize));
    SETTINGS.clearSdFontFamily();
    SETTINGS.fontPointSize = expectedPointSize;
    if (!SETTINGS.saveToFile()) LOG_ERR("FONT", "Failed to restore reader point size after font load failure");
  }

  activityManager.goToReader(APP_STATE.openEpubPath);
  activityManager.loop();
  if (!activityManager.isReaderActivity()) {
    LOG_ERR("FONT", "Cannot resume automatic font install: reader allocation failed");
    activityManager.goHome();
    return;
  }

  if (!fontReady) {
    auto error = makeUniqueNoThrow<FontDownloadActivity>(renderer, mappedInputManager,
                                                         FontDownloadActivity::Purpose::ReaderAutoInstall,
                                                         FontDownloadActivity::StartMode::ResumeFontLoadError);
    if (!error) {
      LOG_ERR("FONT", "OOM allocating resumed FontDownloadActivity (%zu bytes)", sizeof(FontDownloadActivity));
      return;
    }
    activityManager.pushActivity(std::move(error));
    activityManager.loop();
    return;
  }

  auto textSettings = makeUniqueNoThrow<TextSettingsActivity>(
      renderer, mappedInputManager, &sdFontSystem.registry(), TextSettingsActivity::Tab::Family,
      TextSettingsActivity::InitialFontState::Changed, TextSettingsActivity::StartMode::AskThenExit);
  if (textSettings) {
    activityManager.pushActivity(std::move(textSettings));
    activityManager.loop();
    return;
  }

  LOG_ERR("FONT", "OOM allocating automatic TextSettingsActivity (%zu bytes)", sizeof(TextSettingsActivity));
  SETTINGS.sdFontFlashPreload = 0;
  if (!SETTINGS.saveToFile()) LOG_ERR("FONT", "Failed to persist disabled font preload after OOM");

  auto notice = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInputManager, "", tr(STR_FONT_PRELOAD_FAILED),
                                                        ConfirmationActivity::BodyPlacement::PopupTitle);
  if (!notice) {
    LOG_ERR("FONT", "OOM allocating font preload failure notice (%zu bytes)", sizeof(ConfirmationActivity));
    return;
  }
  activityManager.pushActivity(std::move(notice));
  activityManager.loop();
}

void setup() {
  BoardConfig::holdPowerRails();

  // --- Read Pico boot order (read-pico.md 1.4) ------------------------------
  // This board's bring-up is ordered by its power tree, and the reference
  // firmware uses the same order (read_pico_init.c: board I2C + expander and the
  // touch reset first, then the EPD, then the accelerometer/touch drivers):
  //
  //   1. shared I2C, SDA39/SCL40 at 400 kHz
  //   2. FCA9555 expander + PMU handshake + CST836U reset pulse
  //   3. SY7636A EPD rails, then the panel itself
  //   4. touch backend (and the IMU, which this board does not use)
  //   5. SD card
  //
  // 1-2 is gpio.begin() below (BoardReadPico::begin()): nothing downstream can
  // be sequenced before the expander answers, because it enables the SY7636A
  // (P0.3), drives XOE (P0.1), gates the touch reset (P0.7), senses PGOOD (P0.5)
  // and carries the card detect (P0.6). 3-4 is setupDisplayAndFonts(): the
  // SY7636A sequence is the board's LgfxEpdPowerHooks inside display.begin(),
  // and gpio.beginInput() brings the CST836U up immediately after it.
  //
  // 5 (SD) deliberately stays where it is, ahead of the panel: storage is a
  // boot-time dependency in CrossMux — SETTINGS/APP_STATE/reading state are read
  // from it before any activity exists, and the SD-failure path itself paints a
  // screen — and this board's SDMMC pins (CLK38/CMD42/D0=44) share nothing with
  // the shared I2C bus (39/40) or the EPD bus (3..21, 45..48), so mounting late
  // would reorder shared setup code for every target with no hardware reason.
  // The board only needs the expander to be up first, which gpio.begin() does.
  //
  // Every step degrades instead of aborting: a dead expander leaves the panel
  // dark and the keys dead (logged), a missing card shows the SD error screen,
  // and a failed PMU handoff leaves battery/RTC/power-off reporting unknown.
#ifdef ENABLE_SERIAL_LOG
#ifdef CROSSPOINT_WAIT_FOR_USB_SERIAL
  // Development builds preserve reliable early CDC logs; release builds let
  // enumeration proceed asynchronously so users do not pay this startup cost.
  delay(250);
#endif
#if !FREEINK_DEVICE_METALIO_PLUS
  Serial.begin(115200);
#endif
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
  const bool otaPendingAtBoot = HalOtaSlot::runningImageState() == HalOtaSlot::RunningImageState::PendingVerify;

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const bool targetIsValid = isSilentReboot && silentRebootTarget < static_cast<uint32_t>(SilentRebootTarget::Count);
  SilentRebootTarget snapshotTarget =
      targetIsValid ? static_cast<SilentRebootTarget>(silentRebootTarget) : SilentRebootTarget::Home;
  const bool fontPointSizeIsValid = snapshotTarget == SilentRebootTarget::ReaderPreloadChineseFont &&
                                    silentRebootFontPointSize > 0 && silentRebootFontPointSize <= UINT8_MAX;
  const uint8_t snapshotFontPointSize =
      snapshotTarget == SilentRebootTarget::ReaderPreloadChineseFont && fontPointSizeIsValid
          ? static_cast<uint8_t>(silentRebootFontPointSize)
          : 0;
  if (snapshotTarget == SilentRebootTarget::ReaderPreloadChineseFont && snapshotFontPointSize == 0) {
    snapshotTarget = SilentRebootTarget::Home;
  }
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  silentRebootFontPointSize = 0;
  if (snapshotTarget == SilentRebootTarget::ReaderSuppressFontPrompt ||
      snapshotTarget == SilentRebootTarget::ReaderPreloadChineseFont) {
    FontDownloadActivity::suppressChineseFontPromptThisBoot();
  }
  const bool silentRebootLightOn = isSilentReboot && (silentRebootPayload & SILENT_REBOOT_LIGHT_ON) != 0;
  silentRebootPayload = 0;

  // On Read Pico this pair is steps 1-2 of the boot order above: gpio.begin()
  // runs BoardReadPico::begin() (shared I2C, FCA9555 self-test + Port-0 config,
  // CST836U reset pulse, PMU handshake, accelerometer identity probe) before
  // anything else on that board is touched, and it deliberately leaves that
  // board's input backends unstarted. powerManager.begin() installs the same
  // board's PMU host-shutdown hook, so it has to follow. Every other target is
  // unchanged.
  gpio.begin();
  powerManager.begin();

  const auto wakeupReason = gpio.getWakeupReason();
  // Sample the wake hold now — a click wake is released within milliseconds of
  // boot — but defer the sleep-or-boot decision until SETTINGS is loaded below:
  // click-to-wake is a setting, and an X4 battery power-off cuts all power, so
  // only SD state survives to the next boot.
  const bool wakeHoldVerified = wakeupReason != HalGPIO::WakeupReason::PowerButton || gpio.verifyPowerButtonWakeup();

  // IMU bring-up on every target. Read Pico's SC7A20H is an accelerometer with no
  // gyroscope, so HalTiltSensor differentiates the gravity component for the tilt
  // page-turn gesture instead of reading an angular rate; the thresholds are converted
  // from the same 270 dps. halClock.begin() restores the system clock from the board RTC
  // when the system clock is invalid; Read Pico's RTC is the PMU, whose hooks
  // BoardReadPico::begin() already installed.
  halTiltSensor.begin();
  halClock.begin();

  LOG_INF("MAIN", "Hardware detect: %s", BoardConfig::ACTIVE.name);

  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    const uint8_t recoveryButton =
        (BoardConfig::isX4Pro() || FREEINK_DEVICE_X4CLASSIC) ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
    recoveryFirmwareMode = gpio.isPressed(recoveryButton);
    if (recoveryFirmwareMode) {
      LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)",
              (BoardConfig::isX4Pro() || FREEINK_DEVICE_X4CLASSIC) ? "DOWN" : "UP");
    }
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  // Read Pico step 5: 1-bit SDMMC, mounted by attempt. The FCA9555 card-detect
  // line (P0.6) is a hint for the failure log only and never a mount gate
  // (read-pico.md B11), so a card that the expander misreads still mounts, and an
  // unreadable card lands here — the error screen below is the controlled path,
  // nothing aborts.
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    const bool fontsReady = setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    activityManager.requestUpdateAndWait();
    if (otaPendingAtBoot && fontsReady) {
      if (HalOtaSlot::confirmRunningImage()) {
        LOG_INF("OTA", "Running image confirmed after SD error display");
      } else {
        LOG_ERR("OTA", "Running image confirmation failed after SD error display");
      }
    }
    return;
  }

#if RICKYOS_PLUS_NETTEST_HEADLESS
  // Diagnostic only: isolate radio/TCP/TLS from the renderer and application
  // activities. Board power and SD are ready; saved network data remains on SD.
  halClock.setAutoSyncEnabled(false);
  LOG_INF("NETTEST", "headless isolation: UI and background clock not started");
  extern void plusNetSelfTestStart();
  plusNetSelfTestStart();
  return;
#endif

  HalSystem::checkPanic();

  const bool settingsLoaded = SETTINGS.loadFromFile();
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  if (!settingsLoaded) SETTINGS.readerMenuStyle = CrossPointSettings::READER_MENU_LIST;
#endif
  const auto onboardingMode =
      settingsLoaded ? LanguageSelectActivity::Mode::Upgrade : LanguageSelectActivity::Mode::Initial;
  const bool requiresOnboarding = CrossPointSettings::requiresOnboarding(SETTINGS.onboardingVersion);
#ifndef SIMULATOR
  halClock.setUseChinaServers(SETTINGS.contentProfile == CrossPointSettings::ContentProfile::China);
#endif
  halClock.setAutoSyncEnabled(SETTINGS.clockAutoSync != 0);
  APP_STATE.loadFromFile();
  const bool isSleepWake = wakeupReason == HalGPIO::WakeupReason::PowerButton;
  const bool isPersistedSleepWake = isSleepWake && !APP_STATE.showBootScreen;

  timezones::applyToClock();
  const bool recentsLoaded = RECENT_BOOKS.loadFromFile();
  if (!recoveryFirmwareMode && !HalSystem::isRebootFromPanic()) UserGuide::prepare(recentsLoaded);
  READING_STATS.loadFromFile();
  // Fixed folders, and the one-time move of legacy books into /books. Runs after
  // recents and statistics load so their paths follow the moved books.
  if (!recoveryFirmwareMode) {
    RickyStorageLayout::ensureFolders();
    RickyStorageLayout::migrateLegacyBooks();
  }
  ACHIEVEMENTS.loadFromFile();
#if RICKYOS_PRODUCT
  rickyBadgeNotice::seedOnce();
#endif
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  // Restore the monotonic clock floor before anything reads time() (event
  // timestamps, loan-expiry checks).
  trustedtime::init();

  // Brightness and warmth are always restored. A normal wake starts with the
  // light off unless Restore Light on Wake is enabled; silent maintenance
  // reboots replay the live state captured at restart, so they neither go dark
  // nor light up against the user's wake preference.
  const bool restoreLightOn =
      isSilentReboot ? silentRebootLightOn : (SETTINGS.frontlightOn != 0 && SETTINGS.frontlightRestoreOnWake != 0);
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      // With Short Power Button Press = Sleep, a single click wakes on any
      // device; otherwise the button must still be held (ghost-wake debounce).
      if (!wakeHoldVerified && SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::SLEEP) {
        LOG_DBG("MAIN", "Power-button wake not held through verification, sleeping");
        Storage.prepareForDeepSleep();
        powerManager.startDeepSleep(gpio);
      }
      wakePowerReleasePending = true;
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // Most devices return to sleep after a USB-powered cold boot.
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
#if FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_EEGO_A4 || \
    FREEINK_DEVICE_WAVESHARE_EPAPER_397 || FREEINK_DEVICE_METALIO_EINK4 || FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
      // X4 Pro must stay awake so USB Serial/JTAG remains available after leaving
      // USB Drive and reconnecting the cable. Paper Mono has no armable GPIO wake
      // (its button is behind the PMIC). EEGO A4's post-flash reset reads as
      // POWERON (native-USB), so a flash would otherwise be misclassified as a
      // USB-power cold boot and sleep. Waveshare 3.97 hits both: its side key is
      // behind the AXP2101 (input.power == PIN_UNASSIGNED) and it is a native-USB
      // S3, so startDeepSleep() there is a PMIC shutdown on every cabled boot.
      // Read Pico is the same class of device and would be worse: its power key is
      // PMU-owned (input.power == PIN_UNASSIGNED), it is a native-USB S3 whose
      // console is USB Serial/JTAG, and its "off" is a PMU host shutdown whose only
      // wake sources are the PMU key / AC-in / RTC alarm — so a cabled boot with a
      // charging battery (isUsbConnected() reads the PMU charge state) would drop
      // the EN rail immediately and look like a boot loop to the user.
      break;
#else
      Storage.prepareForDeepSleep();
      powerManager.startDeepSleep(gpio);
      break;
#endif
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
#ifdef RICKYOS_PRODUCT
  LOG_INF("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);
#else
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);
#endif

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  // Only a verified deep-sleep wake may use the one-shot persisted flag.
  // Otherwise a stale flag could suppress the splash on a cold boot.
  const BootResume resume = isSilentReboot         ? BootResume::Silent
                            : isPersistedSleepWake ? BootResume::SplashlessWake
                                                   : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;
  bool needsWakeRefresh = false;

  const bool fontsReady = setupDisplayAndFonts(resume != BootResume::Splash,
                                               snapshotTarget == SilentRebootTarget::ReaderPreloadChineseFont);
  const bool postOtaBoot = otaPendingAtBoot && fontsReady && activityManager.goToPostOtaBoot(!recoveryFirmwareMode);

  if (!postOtaBoot) {
    switch (resume) {
      case BootResume::Silent:
        // Splash skipped: the routing block below picks the target activity; the
        // panel keeps showing the pre-reboot popup until that first paint lands.
        break;
      case BootResume::SplashlessWake:
        // One-shot flag: re-arm the splash for the next ordinary boot. Save
        // before any painting so a hang in the blocking paint path can't strand
        // us in a splashless-with-no-frame loop on the next boot.
        APP_STATE.showBootScreen = true;
        APP_STATE.saveToFile();
        if (Storage.exists(SLEEP_FRAME_FILE) && loadSleepFrameBuffer()) {
          if (gpio.deviceIsX3()) {
            // begin() clears the X3 controller RAM, so restore the saved frame as
            // the baseline for the first reader paint without refreshing the panel.
            renderer.cleanupGrayscaleWithFrameBuffer();
            allowFastInitialReaderRefresh = true;
          }
        } else {
          // Clean the retained sleep image as part of the first Home paint.
          needsWakeRefresh = true;
        }
        break;
      case BootResume::Splash:
        activityManager.goToBoot();
        break;
    }
  }

  // On a boot screen (cold boot or after an update), before any page needs it: index the
  // library and refresh recent covers when this firmware or the card calls for it, so the
  // first visits are not the slow ones. Wakes and silent restarts stay instant.
  if (!recoveryFirmwareMode && !HalSystem::isRebootFromPanic() && !requiresOnboarding &&
      (postOtaBoot || resume == BootResume::Splash)) {
    BootPreparation::run(renderer);
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivityWith<SdFirmwareUpdateActivity>(/*recoveryMode=*/true);
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (postOtaBoot) {
    if (requiresOnboarding) {
      activityManager.replaceActivityWith<LanguageSelectActivity>(onboardingMode);
    } else {
      activityManager.goHome();
    }
  } else if (requiresOnboarding) {
    activityManager.replaceActivityWith<LanguageSelectActivity>(onboardingMode);
  } else if (resume == BootResume::Silent && snapshotTarget == SilentRebootTarget::ReaderPreloadChineseFont) {
    continueChineseFontInstall(snapshotFontPointSize);
  } else if (resume == BootResume::Silent &&
             (snapshotTarget == SilentRebootTarget::Reader ||
              snapshotTarget == SilentRebootTarget::ReaderSuppressFontPrompt) &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent && snapshotTarget == SilentRebootTarget::JoinNetwork) {
    activityManager.goToJoinNetwork();
  } else if (resume == BootResume::Silent && snapshotTarget == SilentRebootTarget::Settings) {
    // Back out of the WiFi rows and the user is where they left off, not on Home.
    activityManager.goToSettings();
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    if (needsWakeRefresh) renderer.requestNextRefresh(HalDisplay::HALF_REFRESH);
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    // Splashless wake leaves the retained sleep frame on the panel; without a
    // clean first paint the reader shows the previous screen's residue (A4
    // grayscale panels ghost worst). Mirror the Home branch's HALF refresh so
    // reader resume also clears the retained frame.
    if (needsWakeRefresh) renderer.requestNextRefresh(HalDisplay::HALF_REFRESH);
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
  }

  if (resume == BootResume::Silent) {
    if (postOtaBoot) {
      // Apply the queued Home replacement before waiting for its first physical paint.
      activityManager.loop();
    }
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }


  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;
#if RICKYOS_PLUS_NETTEST && !RICKYOS_PLUS_NETTEST_CONNECTED
  extern void plusNetSelfTestStart();
  plusNetSelfTestStart();
#endif
}

void loop() {
#if RICKYOS_PLUS_POWER_LOG
  {
    static bool booted = false;
    static uint32_t lastSample = 0;
    if (!booted) {
      booted = true;
      lastSample = millis();
      powerLog("boot");
    } else if (millis() - lastSample >= 60000) {
      lastSample = millis();
      powerLog("tick");
    }
  }
#endif
#if RICKYOS_PLUS_RADIO_AUTOSTART
  // Explicit diagnostic build only; run once after the normal UI has started.
  static bool radioAutoStarted = false;
  if (!radioAutoStarted && millis() >= 15000) {
    radioAutoStarted = true;
    extern void plusRadioAutoStart(GfxRenderer& renderer);
    plusRadioAutoStart(renderer);
  }
#endif
#if RICKYOS_PLUS_NETTEST_HEADLESS
  delay(250);
  return;
#endif
#ifdef RICKYOS_STANDBY_DIAG
  standby_diag::dumpWhenAwake();
#endif
#if !defined(SIMULATOR)
  {
    // After a software restart, replay the previous session's last log lines once a
    // serial host has had time to reconnect (USB 传书 owned the port during that session).
    static bool previousLogsShown = false;
    if (!previousLogsShown && millis() > 8000) {
      previousLogsShown = true;
      const std::string previous = HalSystem::takePreviousSessionLogs();
      if (!previous.empty()) {
        LOG_INF("MAIN", "previous session's last logs:");
        logSerial.print(previous.c_str());
      }
    }
  }
#endif
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

#if FREEINK_CAP_TOUCH && FREEINK_DEVICE_READPICO
  if (sdFontReloadPending) {
    sdFontReloadPending = false;
    const unsigned long reloadStart = millis();
    LOG_DBG("MAIN", "Reloading SD font family after the network session");
    {
      // The independent render task shares the font maps and resident font objects.
      RenderLock lock;
      sdFontSystem.ensureLoaded(renderer);
    }
    LOG_DBG("MAIN", "SD font reload after network took %lu ms", millis() - reloadStart);
    activityManager.requestUpdate();
  }
#endif

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  mappedInputManager.update();
  if (activityManager.standbyShowing()) mappedInputManager.suppressStandbyHomeActions();
#if FREEINK_DEVICE_READPICO || FREEINK_DEVICE_METALIO_PLUS
  // Cancel before any handler can wait for the framebuffer lock. These are
  // snapshot queries: they do not consume the gesture from its normal owner.
#if CROSSPOINT_EMULATED
  const bool physicalPress = gpio.wasAnyPressed();
#else
  const bool physicalPress = gpio.physicalPressedMask() != 0;
#endif
  if (mappedInputManager.wasAnyPressed() || mappedInputManager.wasAnyReleased()) {
    activityManager.cancelIdleRender("input", true);
  } else if (physicalPress) {
    activityManager.cancelIdleRender("key held", true);
  } else if (gpio.wasTouchActivity()) {
    activityManager.cancelIdleRender("touch", true);
  }
#endif
#if FREEINK_DEVICE_METALIO_PLUS
  gpio.updateHapticFeedback(activityManager.standbyShowing() ? 0 : SETTINGS.plusTouchFeedback);
#elif FREEINK_CAP_HAPTIC
  gpio.updateHapticFeedback(SETTINGS.hapticFeedbackLevel);
#endif
  updateBluetoothLifecycle();

  static bool bluetoothWasConnected = false;
  const bool bluetoothConnected = bleinput::isConnected();
  if (bluetoothConnected != bluetoothWasConnected) {
    bluetoothWasConnected = bluetoothConnected;
    bleinput::logDiagnostics(bluetoothConnected ? "connected" : "disconnected");
    if (activityManager.isReaderActivity()) activityManager.requestUpdate();
  }

#if CROSSPOINT_CAP_SOUND_FEEDBACK
  SoundFeedback::update(SETTINGS.soundFeedbackLevel, gpio.physicalPressedMask());
#endif

#if FREEINK_DEVICE_METALIO_PLUS
  // Charge qualification must progress even when no page is repainted. This
  // bounded poll uses NVS/I2C, not SD, and only runs while this loop is awake.
  powerManager.pollBatteryWhileAwake();
#endif

  if (activityManager.requiresExclusiveStorageLoop()) {
    // USB Drive handed the raw SD card to the host. Do not run screenshots,
    // sleep, shortcuts, or normal navigation while its filesystem is detached.
    activityManager.loop();
    if (activityManager.preventAutoSleep()) {
      powerManager.setPowerSaving(false);
      delay(10);
    } else {
      // No host is active, so a slower loop is safe. The activity itself times
      // out the raw-storage handoff rather than entering deep sleep detached.
      powerManager.setPowerSaving(true);
      delay(50);
    }
    return;
  }

  {
    int touchX = 0, touchY = 0;
    if (mappedInputManager.isScreenTouchHeld(touchX, touchY) || mappedInputManager.wasScreenTouchReleased())
      halTiltSensor.noteTouch();
  }
  // Back double tap waits for device tuning (see SettingsList): off whatever is stored.
  halTiltSensor.update(SETTINGS.effectiveTiltPageTurn(), SETTINGS.orientation, activityManager.isReaderActivity(),
                       CrossPointBackTap::TAP_OFF, SETTINGS.backTapSensitivity);
  halClock.update();

  renderer.setFadingFix(SETTINGS.fadingFix);

#if FREEINK_DEVICE_METALIO_PLUS && RICKYOS_PLUS_FIELD_DIAG
  plusFieldDiagnosticsPoll();
#endif

  // Plus logs through IDF, not Arduino Serial (Logging.cpp). Its console and
  // the ROM console do not depend on Arduino USB CDC's connection state.
  // Never while a page renders or drives the panel: the largest-free-block
  // queries walk each heap under its lock, interrupts off, for hundreds of
  // microseconds on the 16 MB PSRAM heap. On the Plus that stalled the panel's
  // bounce-buffer refill, which then re-sent rows 16 back: a faint copy of the
  // text beside it at the portrait left edge. The snapshot waits for the next
  // idle loop instead.
  if ((FREEINK_DEVICE_METALIO_PLUS || Serial || FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_ROM_PRINTF) &&
      millis() - lastMemPrint >= 10000 && !RenderLock::peek()) {
    const auto heap = HalMemory::getInternalHeap();
    LOG_INF("MEM", "Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes", heap.freeBytes,
            heap.totalBytes, heap.minFreeBytes, heap.largestBlockBytes);
#if FREEINK_DEVICE_METALIO_PLUS
    // DMA is a subset of internal RAM, not an additional pool. Track its
    // contiguous capacity separately; a large PSRAM heap cannot satisfy it.
    const auto dma = HalMemory::getInternalDmaHeap();
    LOG_INF("MEM", "DMA: Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes", dma.freeBytes,
            dma.totalBytes, dma.minFreeBytes, dma.largestBlockBytes);
#endif
#ifdef BOARD_HAS_PSRAM
    const auto psram = HalMemory::getPsramHeap();
    LOG_INF("MEM", "PSRAM: Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes",
            psram.freeBytes, psram.totalBytes, psram.minFreeBytes, psram.largestBlockBytes);
#endif
    if (bleinput::isRunning()) bleinput::logDiagnostics("running");
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any real user activity (button, touch, or tilt).
  static unsigned long lastActivityTime = millis();
  if (mappedInputManager.wasAnyPressed() || mappedInputManager.wasAnyReleased() || gpio.wasTouchActivity() ||
      halTiltSensor.hadActivity()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }
  // Standby closes itself on a key press that woke its light sleep, which this loop
  // never saw as input: leaving Standby is activity, or idle would reopen it at once.
  static bool standbyWasShowing = false;
  const bool standbyShowingNow = activityManager.standbyShowing();
  if (standbyWasShowing && !standbyShowingNow) lastActivityTime = millis();
  standbyWasShowing = standbyShowingNow;
  // preventAutoSleep() is intentionally NOT folded into the activity check above:
  // it only short-circuits the deep-sleep timer below, not the inactivity clock
  // that drives auto-downclock. Standby (a clock face) wants deep sleep blocked
  // but still benefits from the framework dropping CPU to LOW_POWER_FREQ.

  if (wakePowerReleasePending && !gpio.isPressed(HalGPIO::BTN_POWER)) {
    wakePowerReleasePending = false;
    return;
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  if (handleX4ProFrontlightDoubleClick()) return;

  const bool x4ProDoubleClickPwrLight = BoardConfig::isX4Pro() && SETTINGS.doubleClickPwrLight;

#if FREEINK_CAP_TOUCH
  mappedInputManager.setPowerConfirmClickFrame(false);
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM && x4ProDoubleClickPwrLight) {
    if (lastX4ProPowerClickAt != 0 && millis() - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
      lastX4ProPowerClickAt = 0;
      mappedInputManager.setPowerConfirmClickFrame(true);
    }
    // A release held too long to be a double-click candidate (but still within
    // the normal Confirm press duration) never reaches handleX4ProFrontlightDoubleClick's
    // click tracking above, so it needs its own Confirm check here.
    if (mappedInputManager.wasReleased(MappedInputManager::Button::Power) &&
        gpio.getPowerButtonHeldTime() > X4PRO_POWER_CLICK_MAX_HOLD_MS &&
        gpio.getPowerButtonHeldTime() <= SETTINGS.getPowerButtonDuration()) {
      mappedInputManager.setPowerConfirmClickFrame(true);
    }
  }
#endif

  // Same deferral for SLEEP: getPowerButtonDuration() drops to 10ms so a quick
  // tap sleeps the device, which otherwise fires on button-down and never lets
  // a second click land. Sleep only once the double-click window has passed.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP && x4ProDoubleClickPwrLight &&
      lastX4ProPowerClickAt != 0 && millis() - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = 0;
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // RickyOS power model: Standby keeps the device on (the page under it stays in
  // memory, the clock light-sleeps between minutes); power-off is the PMU cut, whose
  // only way back is holding the side key. Standby asks for the power-off itself
  // once it has run for the auto power-off time.
  if (activityManager.consumePowerOffRequest()) {
    LOG_DBG("SLP", "Standby ran its auto power-off time");
    enterDeepSleep(true);
    return;
  }
  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
#if RICKYOS_PLUS_NETTEST
  extern bool plusNetSelfTestActive();
  if (plusNetSelfTestActive()) lastActivityTime = millis();  // the radio test must not light-sleep
#endif
  if (sleepTimeoutMs > 0 && !activityManager.preventAutoSleep() && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-standby after %lu ms of inactivity", sleepTimeoutMs);
    STANDBY_DIAG("auto-standby after %lus idle", sleepTimeoutMs / 1000);
    activityManager.openStandby();
    lastActivityTime = millis();
    return;
  }

  // A hold that woke the device must be released before it can count as a new
  // in-app long press. Otherwise a user who keeps holding after wake would put
  // the device straight back to sleep once allowSleepAt expires.
  static bool powerReleasedSinceWake = false;
  if (!gpio.isPressed(HalGPIO::BTN_POWER)) powerReleasedSinceWake = true;

  // On X4 Pro with SLEEP, a press still within the click window is a
  // double-click candidate — let it be released and evaluated above instead
  // of sleeping on button-down.
  const bool x4ProAwaitingClickWindow = x4ProDoubleClickPwrLight &&
                                        SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP &&
                                        gpio.getPowerButtonHeldTime() <= X4PRO_POWER_CLICK_MAX_HOLD_MS;

  // Side key: a short press toggles Standby, a hold powers off (the same hold turns
  // the device back on). The PMU reports the key over the I2C bus it shares with
  // touch, and a failed poll reads as released: a release only counts after the key
  // has read up for kSideKeyReleaseMs, so a hold is not split into short presses.
  // The press that powered the device on is ignored until it is really let go.
  {
    constexpr unsigned long kSideKeyReleaseMs = 200;
    static bool keyDown = false;
    static bool holdHandled = false;
    static bool wakePressPending = true;  // the key may still be held from power-on
    static unsigned long pressedAt = 0;
    static unsigned long lastDownAt = 0;
    const unsigned long now = millis();
    const bool rawDown = gpio.isPressed(HalGPIO::BTN_POWER) && !gpio.isPressed(HalGPIO::BTN_DOWN);
    if (rawDown) {
      if (!keyDown) {
        keyDown = true;
        pressedAt = now;
        holdHandled = false;
      }
      lastDownAt = now;
      if (!wakePressPending && !holdHandled && now >= allowSleepAt && now - pressedAt >= RICKY_POWER_OFF_HOLD_MS) {
        holdHandled = true;
        LOG_DBG("MAIN", "Side key held %lums, powering off", now - pressedAt);
        enterDeepSleep();
        return;
      }
    } else if (keyDown && now - lastDownAt >= kSideKeyReleaseMs) {
      keyDown = false;
      const bool wakePress = wakePressPending;
      wakePressPending = false;
      if (!wakePress && !holdHandled && now >= allowSleepAt) {
        lastActivityTime = now;
        STANDBY_DIAG("side key short press (%lums), standby=%d", lastDownAt - pressedAt,
                     activityManager.standbyShowing());
        if (activityManager.standbyShowing()) {
          activityManager.closeStandby();
        } else {
          activityManager.openStandby();
        }
        return;
      }
    } else if (!keyDown) {
      wakePressPending = false;  // booted without the key held
    }
#if CROSSPOINT_EMULATED == 0
    HalPowerManager::setSideKeyBusy(keyDown);
#endif
  }
  if (!kRickyPowerModel && !x4ProAwaitingClickWindow && powerReleasedSinceWake && millis() >= allowSleepAt &&
      gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    LOG_DBG("MAIN", "Power button held %lums, sleeping", gpio.getPowerButtonHeldTime());
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (mappedInputManager.homeButtonAction() == HomeButtonAction::ToggleFrontlight) {
    toggleFrontlight();
  }
  if (mappedInputManager.homeButtonAction() == HomeButtonAction::Standby) activityManager.openStandby();
  if (mappedInputManager.homeButtonAction() == HomeButtonAction::HoldingHand) {
    SETTINGS.pageTurnDirection = SETTINGS.pageTurnDirection == 0 ? 1 : 0;
    SETTINGS.saveToFile();
    activityManager.requestUpdate();  // touch zones and any hand-dependent chrome follow at once
  }
  if (mappedInputManager.homeButtonAction() == HomeButtonAction::Refresh ||
      (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
       mappedInputManager.wasReleased(MappedInputManager::Button::Power))) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    if (!activityManager.handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  // Not while reading: there a repaint is a full page re-render (visible
  // flash, the AA pass re-running, and a frontlight dip under the refresh
  // load); the reader's status bar picks the charging state up on the next
  // page turn instead.
  // Full-screen Standby has no battery icon or input hints to update.
  if (gpio.wasUsbStateChanged() && !activityManager.isReaderActivity() && !activityManager.standbyShowing()) {
    activityManager.requestUpdate();
  }
#ifndef CROSSPOINT_EMULATED
  if (gpio.wasInputModalityChanged() && gpio.hasTouch() && UITheme::getInstance().hasMainTabs() &&
      !activityManager.isReaderActivity() && !activityManager.standbyShowing()) {
    activityManager.requestUpdate();
  }
#endif

  const unsigned long activityStartTime = millis();
  const bool readerWasActive = activityManager.isReaderActivity();
  activityManager.loop();
  updateBluetoothLifecycle();
  const bool readerIsActive = activityManager.isReaderActivity();
  const unsigned long activityDuration = millis() - activityStartTime;

  if (readerWasActive && !readerIsActive) {
    if (!READING_STATS.saveToFile()) {
      LOG_ERR("RST", "Failed to save reading stats after reader exit");
    }
    if (!ACHIEVEMENTS.saveToFile()) {
      LOG_ERR("ACH", "Failed to save achievements after reader exit");
    }
  } else if (readerIsActive && (millis() - lastActivityTime) >= READING_STATS_CHECKPOINT_IDLE_MS &&
             !activityManager.skipLoopDelay() && !activityManager.preventAutoSleep() &&
             READING_STATS.shouldSaveCheckpoint()) {
    activityManager.cancelIdleRender("stats checkpoint", true);
    RenderLock lock;
    if (!READING_STATS.saveToFile()) {
      LOG_ERR("RST", "Failed to save idle reading checkpoint");
    }
  }

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  bool skipLoopDelay = false;
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) {
      // Let rendering advance without treating lock contention as idle.
      delay(10);
      return;
    }
    skipLoopDelay = activityManager.skipLoopDelay();
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (skipLoopDelay) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      // Sleep in short slices and wake the poll as soon as a button contact closes.
      // InputManager commits a press only when two consecutive polls agree, so a
      // press shorter than one 50 ms sleep could land in a single sample and be lost.
      const unsigned long idleStart = millis();
      while (millis() - idleStart < 50) {
        delay(10);
        if (gpio.rawInputActive()) break;
      }
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
