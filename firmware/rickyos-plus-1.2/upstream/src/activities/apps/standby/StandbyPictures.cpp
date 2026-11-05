#include "StandbyPictures.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "network/RickyWallpaperCatalog.h"
#include "util/RickyStorageLayout.h"

namespace StandbyPictures {
namespace {
constexpr char kThumbDir[] = "/.crosspoint/standby-thumbs";
// Bounded like the image viewer's previews: start over rather than track ages.
constexpr int kThumbCacheMax = 240;
constexpr char kStandbySource[] = "/.crosspoint/standby-source.txt";
constexpr char kPowerOffSource[] = "/.crosspoint/poweroff-source.txt";
constexpr char kReadingSource[] = "/.crosspoint/reading-background-source.txt";
constexpr char kStickerSource[] = "/.crosspoint/sticker-source.txt";

enum class Kind : uint8_t { Bmp, Jpeg, Png };

// The signature decides, as in the image viewer; the extension is the fallback.
Kind sniff(const std::string& path) {
  uint8_t head[8] = {};
  HalFile file;
  if (Storage.openFileForRead("STANDBY", path.c_str(), file) && file.read(head, sizeof(head)) == sizeof(head)) {
    static constexpr uint8_t kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (memcmp(head, kPng, sizeof(kPng)) == 0) return Kind::Png;
    if (head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) return Kind::Jpeg;
    if (head[0] == 'B' && head[1] == 'M') return Kind::Bmp;
  }
  if (FsHelpers::hasPngExtension(path)) return Kind::Png;
  if (FsHelpers::hasJpgExtension(path)) return Kind::Jpeg;
  return Kind::Bmp;
}

// Files of one folder, names sorted, appended to `out` until it holds kMaxPictures.
// `pngOnly` keeps PNGs, the only pictures a transparent wallpaper can be.
void addFolder(const std::string& folder, std::vector<Entry>& out, const bool pngOnly = false) {
  HalFile dir = Storage.open(folder.c_str());
  if (!dir || !dir.isDirectory()) return;
  std::vector<std::string> names;
  std::vector<uint32_t> sizes;
  char name[256];
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    name[0] = '\0';
    file.getName(name, sizeof(name));
    if (name[0] == '\0' || name[0] == '.' || !FsHelpers::hasImageExtension(name)) continue;
    if (pngOnly && !FsHelpers::hasPngExtension(std::string_view(name))) continue;
    names.emplace_back(name);
    sizes.push_back(static_cast<uint32_t>(file.fileSize()));
  }
  // Sort names, keeping each size with its name.
  std::vector<std::string> sorted = names;
  FsHelpers::sortFileList(sorted);
  for (const auto& entry : sorted) {
    if (static_cast<int>(out.size()) >= kMaxPictures) return;
    const auto it = std::find(names.begin(), names.end(), entry);
    Entry item;
    item.path = folder == "/" ? "/" + entry : folder + "/" + entry;
    item.size = sizes[static_cast<size_t>(it - names.begin())];
    out.push_back(std::move(item));
  }
}

void pruneThumbs() {
  // Once a boot: listing the folder for every thumbnail made each one slower.
  static bool pruned = false;
  if (pruned) return;
  pruned = true;
  HalFile dir = Storage.open(kThumbDir);
  if (!dir || !dir.isDirectory()) return;
  std::vector<std::string> names;
  char name[64];
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    name[0] = '\0';
    entry.getName(name, sizeof(name));
    if (name[0] != '\0' && name[0] != '.') names.emplace_back(name);
  }
  dir.close();
  if (static_cast<int>(names.size()) < kThumbCacheMax) return;
  // Keep the newer half (the folder lists in creation order) instead of starting over.
  const size_t drop = names.size() - kThumbCacheMax / 2;
  for (size_t i = 0; i < drop; ++i) Storage.remove((std::string(kThumbDir) + "/" + names[i]).c_str());
}

void write16(HalFile& out, const uint16_t value) {
  out.write(static_cast<uint8_t>(value & 0xFF));
  out.write(static_cast<uint8_t>(value >> 8));
}

void write32(HalFile& out, const uint32_t value) {
  for (int i = 0; i < 4; ++i) out.write(static_cast<uint8_t>(value >> (i * 8)));
}

// An 8-bit gray, top-down BMP, the format the gray thumbnail converters write.
bool writeGray8Bmp(HalFile& out, const uint8_t* pixels, const int width, const int height) {
  const int stride = (width + 3) / 4 * 4;
  const uint32_t palette = 256 * 4;
  const uint32_t imageSize = static_cast<uint32_t>(stride) * static_cast<uint32_t>(height);
  out.write('B');
  out.write('M');
  write32(out, 14 + 40 + palette + imageSize);
  write32(out, 0);
  write32(out, 14 + 40 + palette);
  write32(out, 40);
  write32(out, static_cast<uint32_t>(width));
  write32(out, static_cast<uint32_t>(-height));
  write16(out, 1);
  write16(out, 8);
  write32(out, 0);
  write32(out, imageSize);
  write32(out, 2835);
  write32(out, 2835);
  write32(out, 256);
  write32(out, 256);
  for (int i = 0; i < 256; ++i) {
    const uint8_t gray = static_cast<uint8_t>(i);
    out.write(gray);
    out.write(gray);
    out.write(gray);
    out.write(static_cast<uint8_t>(0));
  }
  static constexpr uint8_t kPad[3] = {};
  for (int y = 0; y < height; ++y) {
    if (out.write(pixels + static_cast<size_t>(y) * width, width) != static_cast<size_t>(width)) return false;
    if (stride > width && out.write(kPad, stride - width) != static_cast<size_t>(stride - width)) return false;
  }
  return true;
}

// A BMP shrunk to cover width×height (box average), so the gallery never reads a
// full-screen picture per tile. Row by row: the source is never held whole.
bool bmpToThumb(const std::string& source, HalFile& out, const int width, const int height) {
  HalFile file;
  if (!Storage.openFileForRead("STANDBY", source.c_str(), file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.rewindToData() != BmpReaderError::Ok) return false;
  const int srcW = bitmap.getWidth();
  const int srcH = bitmap.getHeight();
  if (srcW <= 0 || srcH <= 0) return false;
  // Cover the slot, like the JPEG/PNG thumbnails; the drawer crops the centre.
  const float scale = std::max(static_cast<float>(width) / srcW, static_cast<float>(height) / srcH);
  const int outW = std::max(1, std::min(srcW, static_cast<int>(srcW * scale + 0.5f)));
  const int outH = std::max(1, std::min(srcH, static_cast<int>(srcH * scale + 0.5f)));
  // Scratch for one thumbnail (about 60 KB at 172×306): the picture is written once,
  // after the last row, because bottom-up BMPs deliver rows in reverse.
  auto pixels = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(outW) * outH);
  auto row = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(srcW));
  auto raw = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(bitmap.getRowBytes()));
  auto sums = makeUniqueNoThrow<uint32_t[]>(static_cast<size_t>(outW));
  if (!pixels || !row || !raw || !sums) {
    LOG_ERR("STANDBY", "OOM: thumbnail scratch for %dx%d", outW, outH);
    return false;
  }
  int currentOut = -1;
  int rowsInSum = 0;
  const auto flush = [&]() {
    if (currentOut < 0 || rowsInSum == 0) return;
    const int y = bitmap.isTopDown() ? currentOut : outH - 1 - currentOut;
    uint8_t* dest = pixels.get() + static_cast<size_t>(y) * outW;
    for (int x = 0; x < outW; ++x) dest[x] = static_cast<uint8_t>(sums[x] / static_cast<uint32_t>(rowsInSum));
  };
  for (int sy = 0; sy < srcH; ++sy) {
    if (bitmap.readNextRow(row.get(), raw.get(), nullptr, Bitmap::RowOutput::Gray8) != BmpReaderError::Ok)
      return false;
    const int oy = std::min(outH - 1, static_cast<int>(static_cast<int64_t>(sy) * outH / srcH));
    if (oy != currentOut) {
      flush();
      currentOut = oy;
      rowsInSum = 0;
      std::fill(sums.get(), sums.get() + outW, 0u);
    }
    // Horizontal box: each output column averages the source columns it spans.
    for (int x = 0; x < outW; ++x) {
      const int from = static_cast<int>(static_cast<int64_t>(x) * srcW / outW);
      const int to = std::max(from + 1, static_cast<int>(static_cast<int64_t>(x + 1) * srcW / outW));
      uint32_t total = 0;
      for (int sx = from; sx < to; ++sx) total += row[sx];
      sums[x] += total / static_cast<uint32_t>(to - from);
    }
    ++rowsInSum;
  }
  flush();
  return writeGray8Bmp(out, pixels.get(), outW, outH);
}

std::string readLine(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("STANDBY", path, file)) return {};
  char buffer[256];
  const int count = file.read(buffer, sizeof(buffer) - 1);
  if (count <= 0) return {};
  buffer[count] = '\0';
  std::string line(buffer);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  return line;
}
}  // namespace

// The reader's own folders under /images, sorted; false when /images cannot be read.
static bool userFolders(std::vector<std::string>& folders) {
  const std::string root = RickyStorageLayout::IMAGES;
  HalFile dir = Storage.open(root.c_str());
  if (!dir || !dir.isDirectory()) return !Storage.exists(root.c_str());
  char name[256];
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (!entry.isDirectory()) continue;
    name[0] = '\0';
    entry.getName(name, sizeof(name));
    if (name[0] == '\0' || name[0] == '.') continue;
    const std::string path = root + "/" + name;
    if (path == RickyWallpaperCatalog::STICKER_FOLDER || path == RickyWallpaperCatalog::FOLDER) continue;
    folders.push_back(path);
  }
  dir.close();
  FsHelpers::sortFileList(folders);
  return true;
}

bool list(const Filter filter, std::vector<Entry>& out) {
  out.clear();
  std::vector<std::string> folders;
  if (!userFolders(folders)) return false;
  if (filter != Filter::Defaults) {
    addFolder(RickyStorageLayout::IMAGES, out);
    for (const auto& folder : folders) addFolder(folder, out);
  }
  if (filter != Filter::Mine) addFolder(RickyWallpaperCatalog::FOLDER, out);
  return true;
}

bool listStickers(const Filter filter, std::vector<Entry>& out) {
  out.clear();
  std::vector<std::string> folders;
  if (!userFolders(folders)) return false;
  if (filter != Filter::Defaults) {
    addFolder(RickyStorageLayout::IMAGES, out, true);
    for (const auto& folder : folders) addFolder(folder, out, true);
  }
  if (filter != Filter::Mine) addFolder(RickyWallpaperCatalog::STICKER_FOLDER, out, true);
  return true;
}

std::string thumbnailPath(const Entry& entry, const int width, const int height) {
  // FNV-1a over path, size and slot: a replaced picture or a new layout converts again.
  uint32_t hash = 2166136261u;
  const auto mix = [&hash](const uint8_t byte) { hash = (hash ^ byte) * 16777619u; };
  for (const char c : entry.path) mix(static_cast<uint8_t>(c));
  for (const uint32_t value : {entry.size, static_cast<uint32_t>(width), static_cast<uint32_t>(height)})
    for (int i = 0; i < 4; ++i) mix(static_cast<uint8_t>(value >> (i * 8)));
  char name[64];
  snprintf(name, sizeof(name), "%s/%08x.bmp", kThumbDir, static_cast<unsigned>(hash));
  return name;
}

bool ensureThumbnail(const Entry& entry, const int width, const int height) {
  const std::string path = thumbnailPath(entry, width, height);
  if (Storage.exists(path.c_str())) return true;
  if (!Storage.ensureDirectoryExists(kThumbDir)) return false;
  pruneThumbs();
  // Written aside and renamed, so a conversion cut short is never taken for a thumbnail.
  const std::string part = path + ".part";
  if (Storage.exists(part.c_str()) && !Storage.remove(part.c_str())) return false;
  bool made = false;
  {
    HalFile input, output;
    if (!Storage.openFileForWrite("STANDBY", part, output)) return false;
    switch (sniff(entry.path)) {
      case Kind::Jpeg:
        made = Storage.openFileForRead("STANDBY", entry.path.c_str(), input) &&
               JpegToBmpConverter::jpegFileToThumbBmpStreamWithSize(input, output, width, height);
        break;
      case Kind::Png:
        made = Storage.openFileForRead("STANDBY", entry.path.c_str(), input) &&
               PngToBmpConverter::pngFileToThumbBmpStreamWithSize(input, output, width, height);
        break;
      case Kind::Bmp:
        made = bmpToThumb(entry.path, output, width, height);
        break;
    }
    output.flush();
  }
  made = made && Storage.rename(part.c_str(), path.c_str());
  if (!made) Storage.remove(part.c_str());
  return made;
}

static const char* sourceFile(const Target target) {
  switch (target) {
    case Target::PowerOff:
      return kPowerOffSource;
    case Target::Reading:
      return kReadingSource;
    case Target::Sticker:
      return kStickerSource;
    default:
      return kStandbySource;
  }
}

std::string currentSource(const Target target) {
  return readLine(sourceFile(target));
}

std::string sourceInUse(const Target target) {
  bool shown = false;
  switch (target) {
    case Target::PowerOff:
      shown = SETTINGS.rickyPowerOffScreen == CrossPointSettings::RICKY_POWER_OFF_CUSTOM;
      break;
    case Target::Reading:
      shown = SETTINGS.readingBackground != CrossPointSettings::READING_BACKGROUND_OFF &&
              SETTINGS.readingPaper == CrossPointSettings::READING_PAPER_PICTURE;
      break;
    case Target::Sticker:
      shown = SETTINGS.rickyStandbyFace == CrossPointSettings::RICKY_STANDBY_TRANSPARENT;
      break;
    default:
      shown = SETTINGS.rickyStandbyFace == CrossPointSettings::RICKY_STANDBY_PICTURE;
      break;
  }
  return shown ? currentSource(target) : std::string();
}

void rememberSource(const Target target, const std::string& path) {
  const char* file = sourceFile(target);
  HalFile out;
  if (!Storage.openFileForWrite("STANDBY", file, out)) {
    LOG_ERR("STANDBY", "Cannot record the picture source");
    return;
  }
  out.write(reinterpret_cast<const uint8_t*>(path.data()), path.size());
}

}  // namespace StandbyPictures
