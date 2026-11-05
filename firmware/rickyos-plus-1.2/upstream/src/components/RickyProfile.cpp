#include "RickyProfile.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "CrossPointSettings.h"
#include "RickyDesignUi.h"
#include "RickyPageUi.h"
#include "UITheme.h"
#include "icons/rickyAvatars.h"
#include "util/TimeUtils.h"
#include "util/PanelTileCache.h"

namespace RickyProfile {
namespace {
bool validAvatarBitmap(HalFile& file, Bitmap& bitmap) {
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) return false;
  const size_t offset = file.position();
  const size_t bytes = static_cast<size_t>(bitmap.getRowBytes()) * bitmap.getHeight();
  return offset <= file.fileSize() && bytes <= file.fileSize() - offset;
}
}  // namespace
const char* nickname() { return SETTINGS.rickyNickname[0] ? SETTINGS.rickyNickname : tr(STR_RICKY_PROFILE_DEFAULT); }

const char* homePhrase() { return SETTINGS.rickyHomePhrase[0] ? SETTINGS.rickyHomePhrase : tr(STR_RICKY_HOME_PHRASE); }

bool setHomePhrase(const std::string& text) {
  // The keyboard limits bytes, not glyphs. Reject oversized/control-containing
  // input instead of truncating inside a UTF-8 codepoint; no new heap buffer.
  if (text.size() >= sizeof(SETTINGS.rickyHomePhrase)) return false;
  for (const unsigned char c : text) {
    if (c < 0x20 || c == 0x7f) return false;
  }
  if (text == SETTINGS.rickyHomePhrase) return true;
  char previous[sizeof(SETTINGS.rickyHomePhrase)];
  memcpy(previous, SETTINGS.rickyHomePhrase, sizeof(previous));
  memcpy(SETTINGS.rickyHomePhrase, text.c_str(), text.size() + 1);
  if (SETTINGS.saveToFile()) return true;
  memcpy(SETTINGS.rickyHomePhrase, previous, sizeof(previous));
  return false;
}

namespace {
constexpr char PRESET_PREFIX[] = "@avatar:";
}  // namespace

int presetAvatarIndex() {
  if (SETTINGS.rickyAvatarPath[0] == '\0') return DEFAULT_AVATAR;
  const size_t length = sizeof(PRESET_PREFIX) - 1;
  if (strncmp(SETTINGS.rickyAvatarPath, PRESET_PREFIX, length) != 0) return -1;
  const char* digits = SETTINGS.rickyAvatarPath + length;
  if (digits[0] < '0' || digits[0] > '9' || digits[1] != '\0') return -1;
  const int index = digits[0] - '0';
  return index < RickyAvatars::COUNT ? index : -1;
}

namespace {
// `coverage` (0..15) of ink over what is already on the 16-level frame.
void blendInk(const GfxRenderer& renderer, const int x, const int y, const int coverage) {
  const int paper = renderer.grayscale16Level(x, y);
  const int delta = -paper * coverage;  // toward 0 (ink), over 15
  const int level = paper - ((7 - delta) / 15);
  renderer.drawGrayscale16Pixel(x, y, static_cast<uint8_t>(level * 17));
}

// Moves a pixel toward the line gray by coverage/15; never lightens darker paper.
void blendLine(const GfxRenderer& renderer, const int x, const int y, const int coverage) {
  const int paper = renderer.grayscale16Level(x, y);
  const int target = ricky::ds::gray::kLine;
  if (paper <= target) return;
  const int level = paper - ((paper - target) * coverage + 7) / 15;
  renderer.drawGrayscale16Pixel(x, y, static_cast<uint8_t>(level * 17));
}

// The avatar's circular frame: the same light gray edge as covers, not a black ring.
// On a 16-level frame its edges are graded by how much of each pixel the ring covers
// (distance to the circle, one pixel of falloff each side).
void avatarRing(const GfxRenderer& renderer, const Rect& rect, const int size) {
  const int thickness = ricky::ds::stroke::kCoverEdge;
  if (!renderer.isGrayscale16Active()) {
    renderer.drawRoundedRect(rect.x, rect.y, size, size, thickness, size / 2, true);
    return;
  }
  const float centre = size / 2.0f, outer = size / 2.0f, inner = outer - thickness;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float dx = x + 0.5f - centre, dy = y + 0.5f - centre;
      const float d2 = dx * dx + dy * dy;
      if (d2 < (inner - 1) * (inner - 1) || d2 > (outer + 1) * (outer + 1)) continue;
      const float d = std::sqrt(d2);
      const float in = std::clamp(outer + 0.5f - d, 0.0f, 1.0f) - std::clamp(inner + 0.5f - d, 0.0f, 1.0f);
      const int coverage = static_cast<int>(in * 15 + 0.5f);
      if (coverage) blendLine(renderer, rect.x + x, rect.y + y, coverage);
    }
  }
}
}  // namespace

void drawPresetAvatar(const GfxRenderer& renderer, const Rect& rect, const int index) {
  const int size = std::max(0, std::min(rect.width, rect.height));
  if (!size || index < 0 || index >= RickyAvatars::COUNT) return;
  // Line art like the brand badge: 1:1 at the source size, area-sampled below it. The
  // source is run-length coded ink coverage, read once front to back: each drawn row
  // adds up the source rows it covers, so nothing but two stack rows is needed.
  constexpr int SOURCE = RickyAvatars::SIZE;
  const int drawn = std::min(size, SOURCE);
  const int x0 = rect.x + (size - drawn) / 2, y0 = rect.y + (size - drawn) / 2;
  // The default avatar follows Profile -> Theme; the stored index stays 0.
  const bool themed = index == 0 && SETTINGS.rickyCat();
  const uint8_t* rle = themed ? RickyAvatars::catDefault : RickyAvatars::ALL[index];
  const int rleBytes = themed ? RickyAvatars::CAT_DEFAULT_BYTES : RickyAvatars::BYTES[index];
  const bool gray = renderer.isGrayscale16Active();
  uint16_t columns[SOURCE];
  int next = 0, run = 0, level = 0, sourceY = 0;
  // Only inside the circle: a portrait's shoulders may run past it.
  const float centre = drawn / 2.0f, radius2 = centre * centre;
  for (int y = 0; y < drawn; ++y) {
    const int sy0 = y * SOURCE / drawn, sy1 = std::max(sy0 + 1, (y + 1) * SOURCE / drawn);
    std::fill(columns, columns + SOURCE, 0);
    for (; sourceY < sy1; ++sourceY) {
      for (int sx = 0; sx < SOURCE; ++sx) {
        if (run == 0) {
          run = next < rleBytes ? (rle[next] >> 4) + 1 : SOURCE * SOURCE;
          level = next < rleBytes ? rle[next] & 0x0F : 0;
          ++next;
        }
        --run;
        if (sourceY >= sy0) columns[sx] += level;
      }
    }
    const float dy = y + 0.5f - centre;
    for (int x = 0; x < drawn; ++x) {
      const float dx = x + 0.5f - centre;
      if (dx * dx + dy * dy > radius2) continue;
      const int sx0 = x * SOURCE / drawn, sx1 = std::max(sx0 + 1, (x + 1) * SOURCE / drawn);
      int total = 0;
      for (int sx = sx0; sx < sx1; ++sx) total += columns[sx];
      const int area = (sy1 - sy0) * (sx1 - sx0);
      if (gray) {
        const int coverage = (total + area / 2) / area;
        if (coverage) blendInk(renderer, x0 + x, y0 + y, coverage);
      } else if (total * 16 >= 7 * 15 * area) {  // ink where 7/16 of the source is ink
        renderer.drawPixel(x0 + x, y0 + y, true);
      }
    }
  }
  avatarRing(renderer, Rect{x0, y0, drawn, drawn}, drawn);
}

bool setAvatar(const char* value) {
  PanelTileCache::forget("avatar");  // imports reuse two file names
  char previous[sizeof(SETTINGS.rickyAvatarPath)];
  memcpy(previous, SETTINGS.rickyAvatarPath, sizeof(previous));
  strncpy(SETTINGS.rickyAvatarPath, value, sizeof(SETTINGS.rickyAvatarPath) - 1);
  SETTINGS.rickyAvatarPath[sizeof(SETTINGS.rickyAvatarPath) - 1] = '\0';
  if (SETTINGS.saveToFile()) return true;
  memcpy(SETTINGS.rickyAvatarPath, previous, sizeof(previous));
  return false;
}

void drawAvatar(const GfxRenderer& renderer, const Rect& rect) {
  const int size = std::max(0, std::min(rect.width, rect.height));
  if (!size) return;
  if (const int preset = presetAvatarIndex(); preset >= 0) {
    drawPresetAvatar(renderer, rect, preset);
    return;
  }
  bool painted = false;
  if (SETTINGS.rickyAvatarPath[0]) {
    HalFile file;
    if (Storage.openFileForRead("PROFILE", SETTINGS.rickyAvatarPath, file)) {
      // Bitmap already owns bounded reusable row scratch, not a full image.
      Bitmap bitmap(file);
      if (validAvatarBitmap(file, bitmap)) {
        painted = renderer.drawBitmapCropToFill(bitmap, rect.x, rect.y, size, size);
      }
    }
  }
  if (!painted) {
    // The brand badge stays the product's mark (boot, standby); a user's avatar falls
    // back to the default puppy.
    drawPresetAvatar(renderer, rect, DEFAULT_AVATAR);
    return;
  }
  // Imported photos get one even ring, scaled with the avatar.
  renderer.maskRoundedRectOutsideCorners(rect.x, rect.y, size, size, size / 2);
  avatarRing(renderer, rect, size);
}

void drawCard(UiScreen& screen, const GfxRenderer& renderer, freeink::ui::Rect rect, freeink::ui::ActionId action,
              int value) {
  namespace fui = freeink::ui;
  const auto& theme = screen.theme();
  auto& target = screen.target();
  const int gap = ricky::ds::space::kInlineGap;
  const int pad = ricky::ds::space::kCardPadding;
  RickyPageUi::card(target, rect, false);
  const int size = std::max(0, rect.height - pad * 2);
  drawAvatar(renderer, Rect{rect.x + pad, rect.y + pad, size, size});
  const int bodyHeight = target.lineHeight(theme.bodyText.font);
  const int smallHeight = target.lineHeight(theme.smallText.font);
  const int x = rect.x + pad * 2 + size;
  const int width = rect.right() - x - pad - 24;
  const int top = rect.y + (rect.height - bodyHeight - smallHeight - gap / 2) / 2;
  auto name = theme.bodyText;
  name.bold = true;
  name.maxLines = 1;
  RickyDesignUi::text(renderer,
                      {static_cast<int16_t>(x), static_cast<int16_t>(top), static_cast<int16_t>(width),
                       static_cast<int16_t>(bodyHeight + 4)},
                      nickname(), RickyDesignUi::HEADLINE);
  auto detail = theme.smallText;
  detail.maxLines = 1;
  target.text(fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(top + bodyHeight + gap / 2),
                        static_cast<int16_t>(width), static_cast<int16_t>(smallHeight)},
              tr(STR_RICKY_PROFILE_SUBTITLE), detail);
  RickyPageUi::chevron(target, fui::Rect{static_cast<int16_t>(rect.right() - 24 - pad), rect.y, 24, rect.height});
  if (action != fui::NO_ACTION) screen.frame().hit(rect, action, static_cast<int16_t>(value), fui::InputTouch);
}

bool importAvatar(const std::string& path) {
  if (path.size() > 512 || path.empty() || path.front() != '/') return false;
  const bool png = FsHelpers::hasPngExtension(path);
  const bool jpg = FsHelpers::hasJpgExtension(path);
  if (!png && !jpg && !FsHelpers::hasBmpExtension(path)) return false;
  // Alternate private slots: failure never truncates the currently used avatar.
  constexpr const char* slotA = "/.crosspoint/ricky-avatar-a.bmp";
  constexpr const char* slotB = "/.crosspoint/ricky-avatar-b.bmp";
  const char* target = strcmp(SETTINGS.rickyAvatarPath, slotA) == 0 ? slotB : slotA;
  if (path == target) return false;
  if (!Storage.ensureDirectoryExists("/.crosspoint")) return false;
  bool ok = false;
  {
    HalFile input, output;
    if (!Storage.openFileForRead("PROFILE", path, input) || input.isDirectory() || input.fileSize() == 0 ||
        input.fileSize() > 16 * 1024 * 1024)
      return false;
    if (!png && !jpg) {
      Bitmap bitmap(input);
      if (!validAvatarBitmap(input, bitmap) || input.fileSize() > 2 * 1024 * 1024 || !input.seek(0)) return false;
    }
    if (!Storage.openFileForWrite("PROFILE", target, output)) return false;
    if (png) {
      ok = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(input, output, 128, 128);
    } else if (jpg) {
      ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(input, output, 128, 128);
    } else {
      // Small stack chunk instead of a heap copy of the user's original image.
      uint8_t chunk[512];
      size_t remaining = input.fileSize();
      ok = true;
      while (remaining) {
        const size_t count = std::min(remaining, sizeof(chunk));
        if (input.read(chunk, count) != static_cast<int>(count) || output.write(chunk, count) != count) {
          ok = false;
          break;
        }
        remaining -= count;
      }
    }
    output.flush();
  }
  if (!ok) {
    LOG_ERR("PROFILE", "avatar import failed");
    return false;
  }
  {
    HalFile file;
    if (!Storage.openFileForRead("PROFILE", target, file)) return false;
    Bitmap bitmap(file);
    if (!validAvatarBitmap(file, bitmap)) return false;
  }
  return setAvatar(target);
}
}  // namespace RickyProfile
