"""Compile real renderer/decoders with a recording HAL; no physical display required."""
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import struct
import zlib


def png_fixtures(work):
    """Real PNG containers: no optional imaging package or opaque golden blobs."""
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    for name, color in (('rgba', 6), ('gray-alpha', 4), ('indexed', 3), ('rgb-key', 2), ('gray-key', 0)):
        row = bytearray()
        for x in range(16):
            gray = 17 * x
            alpha = (0, 128, 255)[x % 3]
            row += bytes({6: (gray, gray, gray, alpha), 4: (gray, alpha),
                          3: (x,), 2: (gray, gray, gray), 0: (gray,)}[color])
        data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 16, 2, 8, color, 0, 0, 0))
        if color == 3:
            data += chunk(b'PLTE', bytes(v for x in range(16) for v in (17*x,) * 3))
            data += chunk(b'tRNS', bytes((0, 128, 255)[x % 3] for x in range(16)))
        elif color in (0, 2):
            data += chunk(b'tRNS', struct.pack('>H', 17) * (3 if color == 2 else 1))
        data += chunk(b'IDAT', zlib.compress((b'\0' + row) * 2)) + chunk(b'IEND', b'')
        (work / (name + '.png')).write_bytes(data)


def main():
    ROOT = Path(__file__).resolve().parents[2]
    SIM = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/test/_deps/crosspoint_simulator-src'
    CXX = sys.argv[2] if len(sys.argv) > 2 else 'c++'
    jpeg_candidates = sorted(ROOT.glob('.pio/libdeps/*/JPEGDEC/src/JPEGDEC.cpp'))
    JPEG = (Path(sys.argv[3]) if len(sys.argv) > 3 else
            jpeg_candidates[0].parent if jpeg_candidates else ROOT / 'build/test/_deps/jpegdec-src/src')
    png_candidates = sorted(ROOT.glob('.pio/libdeps/*/PNGdec/src/PNGdec.cpp'))
    PNG = png_candidates[0].parent
    if not (JPEG / 'JPEGDEC.cpp').is_file():
        raise SystemExit('Configure host tests or build a firmware environment to provide the existing JPEGDEC dependency.')

    with tempfile.TemporaryDirectory(prefix='native-gray-') as temp:
        work = Path(temp)
        # Keep the simulator's complete HAL declarations so the entire production
        # renderer compiles; only the native transaction/geometry seams are replaced.
        hal = (SIM / 'src/HalDisplay.h').read_text().replace('EInkDisplay einkDisplay;', '')
        hal = hal.replace('EInkDisplay::DISPLAY_WIDTH', '128').replace('EInkDisplay::DISPLAY_HEIGHT', '32')
        if 'getGrayscaleLevels()' not in hal:
            hal = hal.replace('  void begin();', '''  uint8_t getGrayscaleLevels() const;
          uint8_t* beginGrayscale16();
          bool commitGrayscale16();
          void cancelGrayscale16();
          void begin();''')
        hal = hal.replace('  void begin();',
                          '  void setNextGray16UiRefresh(int,int,int,int,uint8_t,uint8_t);\n  void begin();')
        if 'beginRetainedGrayscale16()' not in hal:
            hal = hal.replace('  void begin();', '  uint8_t* beginRetainedGrayscale16();\n  void begin();')
        (work / 'HalDisplay.h').write_text(hal)
        (work / 'Logging.h').write_text('#pragma once\n#define LOG_DBG(...) ((void)0)\n#define LOG_ERR(...) ((void)0)\n#define LOG_INF(...) ((void)0)\n')
        settings = """#pragma once
    #include <cstdint>
    class CrossPointSettings {
     public:
      enum SLEEP_SCREEN_MODE : uint8_t { LIGHT, CUSTOM };
      enum class SLEEP_SCREEN_COVER_MODE { FIT, CROP };
      enum class SLEEP_SCREEN_COVER_FILTER { NO_FILTER, BLACK_AND_WHITE, INVERTED_BLACK_AND_WHITE };
      SLEEP_SCREEN_COVER_MODE sleepScreenCoverMode = SLEEP_SCREEN_COVER_MODE::FIT;
      SLEEP_SCREEN_COVER_FILTER sleepScreenCoverFilter = SLEEP_SCREEN_COVER_FILTER::NO_FILTER;
      uint8_t sleepScreen = LIGHT;
      int failedSaves = 0;
      bool saveToFile() { if (failedSaves > 0) { --failedSaves; return false; } return true; }
      static CrossPointSettings& getInstance() { static CrossPointSettings value; return value; }
    };
    #define SETTINGS CrossPointSettings::getInstance()
    """
        (work / 'CrossPointSettings.h').write_text(settings)
        activity = (ROOT / 'src/activities/apps/airpage/AirPageActivity.cpp').read_text()
        qr = activity[activity.index('  char displayParams[48];'):activity.index('  uploadUrl_ += displayParams;')]
        (work / 'QrParams.h').write_text('std::string qrParams(GfxRenderer& renderer) {\n' + qr +
                                       '  return displayParams;\n}\n')
        sleep = (ROOT / 'src/activities/boot_sleep/SleepActivity.cpp').read_text()
        placement = sleep[sleep.index('HalDisplay::GrayscaleMode sleepGrayscaleMode('):sleep.index('// Kept separate')]
        placement += sleep[sleep.index('struct BitmapPlacement {'):sleep.index('struct OverlayBmpInfo {')]
        placement += sleep[sleep.index('BitmapPlacement calculateBitmapPlacement('):sleep.index('bool parseOverlayBmpHeader(')]
        custom = sleep[sleep.index('void SleepActivity::renderCustomSleepScreen()'):sleep.index('// Sleep screens paint')]
        bitmap_sleep = sleep[sleep.index('void SleepActivity::renderBitmapSleepScreen('):sleep.index('bool SleepActivity::renderSleepOverlayFile(')]
        probe = """#include <cmath>
    #include <Logging.h>
    #include "components/Rect.h"
    inline bool marksPowerOff() { return false; }
    struct RickyGuiStub {
      static Rect drawRickyStandbyIndicator(const GfxRenderer&, bool = false) { return Rect{0, 0, 0, 0}; }
    } GUI;
    enum class SleepRecentKind { Standard };
    inline bool selectRandomSleepFile(const char*, SleepRecentKind, std::string&) { return false; }
    class SleepProbe {
     public:
      explicit SleepProbe(GfxRenderer& value) : renderer(value) {}
      GfxRenderer& renderer;
      mutable int defaults = 0;
      void renderDefaultSleepScreen() const { ++defaults; }
      void renderCustomSleepScreen() const;
      void renderBitmapSleepScreen(const Bitmap&, bool = false, bool = false) const;
    };
    """
        (work / 'SleepProbe.h').write_text(probe + placement +
            (custom + bitmap_sleep).replace('SleepActivity::', 'SleepProbe::'))
        includes = [work, JPEG, PNG, SIM / 'src', ROOT / 'lib/hal', ROOT / 'lib/GfxRenderer', ROOT / 'lib/EpdFont',
                    ROOT / 'lib/Epub', ROOT / 'lib/Memory', ROOT / 'lib/Utf8', ROOT / 'lib/MiniBidi',
                    ROOT / 'lib/InflateReader', ROOT / 'lib/ZipFile', ROOT / 'lib/Serialization',
                    ROOT / 'lib/uzlib/src', ROOT / 'lib/FsHelpers', ROOT / 'lib/JpegToBmpConverter', ROOT / 'src', ROOT / 'src/activities/apps/airpage']
        sources = [ROOT / name for name in (
            'test/native_grayscale/NativeGrayscaleTest.cpp', 'lib/GfxRenderer/GfxRenderer.cpp',
            'lib/GfxRenderer/Bitmap.cpp', 'lib/GfxRenderer/BitmapHelpers.cpp',
            'lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp',
            'lib/Epub/Epub/converters/PngToFramebufferConverter.cpp',
            'lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp',
            'lib/JpegToBmpConverter/JpegToBmpConverter.cpp', 'lib/Memory/BuildScratch.cpp',
            'lib/Epub/Epub/converters/ImageDimsProbe.cpp',
            'src/activities/apps/airpage/AirPageImageRenderer.cpp',
            'src/activities/apps/airpage/AirPageImageStore.cpp',
            'src/activities/apps/airpage/AirPageWallpaper.cpp',
            'lib/EpdFont/EpdFont.cpp', 'lib/EpdFont/EpdFontFamily.cpp', 'lib/Utf8/Utf8.cpp')]
        sources += [JPEG / 'JPEGDEC.cpp', SIM / 'src/HalStorage.cpp']
        sources += [PNG / 'PNGdec.cpp']
        flags = ['-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections', '-DCROSSPOINT_EMULATED=1',
                 '-DDESTRUCTOR_CLOSES_FILE=1', '-D__LINUX__=1', '-DSIMULATOR=1', '-DPNG_MAX_BUFFERED_PIXELS=16416']
        flags += ['-I' + str(p) for p in includes]
        flags += ['-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections']
        binary = work / 'test'
        objects = []
        for source in PNG.glob('*.c'):
            obj = work / (source.stem + '.o')
            subprocess.run(['cc', '-O1', '-I' + str(PNG), '-c', str(source), '-o', str(obj)], check=True)
            objects.append(obj)
        png_fixtures(work)
        subprocess.run([CXX, *flags, *map(str, sources), *map(str, objects), '-o', str(binary)], check=True)
        subprocess.run([str(binary), str(ROOT / 'test/native_grayscale/gray16.jpg'), str(work)], check=True)

    print('Native BMP/JPEG/transparent PNG, retained baseline, orientations, cancellation, OOM, AirPage and sleep/wallpaper passed')


if __name__ == "__main__":
    main()
