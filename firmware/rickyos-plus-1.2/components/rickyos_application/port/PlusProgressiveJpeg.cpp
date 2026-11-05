#include "PlusProgressiveJpeg.h"

#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace {
constexpr size_t kDecodeBudget = 10u * 1024 * 1024;
constexpr size_t kReserve = 512u * 1024;
constexpr size_t kMaxPixels = 2u * 1024 * 1024;
struct Budget {
  size_t limit = kDecodeBudget;
  size_t used = 0;
  bool oom = false;
  uint32_t yielded = 0;
  CancelCheck cancellation;
};
// Every allocation belongs to one synchronous decode. TLS prevents a different
// task's decoder from borrowing this budget; no global stb flip/gamma options.
thread_local Budget* activeBudget = nullptr;
bool jpegCheckpoint() {
  if (!activeBudget) return false;
  ImageToFramebufferDecoder::yieldDuringDecode(activeBudget->yielded);
  return !activeBudget->cancellation.isCancelled();
}
struct alignas(std::max_align_t) Allocation {
  size_t bytes;
  Budget* owner;
};
void* jpegAlloc(size_t bytes) {
  auto* b = activeBudget;
  if (!b || !bytes || b->cancellation.isCancelled()) return nullptr;
  const auto ram = HalMemory::getPsramHeap();
  if (bytes > b->limit - b->used || ram.freeBytes < bytes + sizeof(Allocation) + kReserve ||
      ram.largestBlockBytes < bytes + sizeof(Allocation)) {
    b->oom = true;
    return nullptr;
  }
  auto memory = HalMemory::allocatePsram(bytes + sizeof(Allocation));
  if (!memory) {
    b->oom = true;
    return nullptr;
  }
  // Ownership crosses the stb C allocation API; jpegFree returns it to the HAL.
  auto* header = reinterpret_cast<Allocation*>(memory.release());
  header->bytes = bytes;
  header->owner = b;
  b->used += bytes;
  return header + 1;
}
void jpegFree(void* p) {
  if (!p) return;
  auto* header = static_cast<Allocation*>(p) - 1;
  header->owner->used -= header->bytes;
  HalMemory::PsramDeleter{}(reinterpret_cast<uint8_t*>(header));
}
void* jpegRealloc(void* p, size_t bytes) {
  if (!p) return jpegAlloc(bytes);
  if (!bytes) {
    jpegFree(p);
    return nullptr;
  }
  auto* next = jpegAlloc(bytes);
  if (!next) return nullptr;
  memcpy(next, p, std::min(bytes, (static_cast<Allocation*>(p) - 1)->bytes));
  jpegFree(p);
  return next;
}
}  // namespace

// Private JPEG-only build. Other formats and no-PSRAM targets retain their
// existing streaming decoders; this does not enable stdio or RGB framebuffers.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 8192
#define STBI_MALLOC(n) jpegAlloc(n)
#define STBI_REALLOC(p, n) jpegRealloc(p, n)
#define STBI_FREE(p) jpegFree(p)
#define STBI_RICKYOS_JPEG_CHECKPOINT() jpegCheckpoint()
#include "vendor/stb_image.h"

namespace {
struct Input {
  HalFile file;
  HalMemory::PsramBuffer buffer;
  size_t start = 0, end = 0;
  uint32_t yielded = 0;
  CancelCheck cancellation;
  bool failed = false;
};
int read(void* user, char* out, int count) {
  auto& input = *static_cast<Input*>(user);
  int done = 0;
  while (done < count && !input.failed && !input.cancellation.isCancelled()) {
    ImageToFramebufferDecoder::yieldDuringDecode(input.yielded);
    if (input.cancellation.isCancelled()) break;
    if (input.start == input.end) {
      const size_t remaining = input.file.size() - input.file.position();
      const size_t request = std::min(remaining, size_t{16384});
      if (!request) break;
      const int got = input.file.read(input.buffer.get(), request);
      if (got != static_cast<int>(request)) {
        input.failed = true;
        break;
      }
      input.start = 0;
      input.end = static_cast<size_t>(got);
    }
    const size_t chunk = std::min(input.end - input.start, static_cast<size_t>(count - done));
    memcpy(out + done, input.buffer.get() + input.start, chunk);
    input.start += chunk;
    done += static_cast<int>(chunk);
  }
  return done;
}
void skip(void* user, int count) {
  auto& i = *static_cast<Input*>(user);
  const int64_t logical = static_cast<int64_t>(i.file.position()) - (i.end - i.start);
  const int64_t target = logical + count;
  if (target < 0 || target > i.file.size() || !i.file.seek(static_cast<size_t>(target))) i.failed = true;
  i.start = i.end = 0;
}
int eof(void* user) {
  auto& i = *static_cast<Input*>(user);
  return i.failed || i.cancellation.isCancelled() || (i.start == i.end && i.file.position() >= i.file.size());
}
const stbi_io_callbacks callbacks{read, skip, eof};
}  // namespace

bool decodePlusProgressiveJpeg(const std::string& path, GfxRenderer& renderer, const RenderConfig& cfg) {
  const auto setError = [&](ImageRenderError error) {
    if (cfg.error) *cfg.error = error;
  };
  setError(ImageRenderError::Failed);
  if (cfg.cancellation.isCancelled()) {
    setError(ImageRenderError::Cancelled);
    return false;
  }
  if (!renderer.isGrayscale16Active() || cfg.output != DecodeOutput::NativeGrayscale16 || activeBudget) return false;
  Input input;
  input.cancellation = cfg.cancellation;
  if (!Storage.openFileForRead("JPG", path, input.file)) return false;
  input.buffer = HalMemory::allocatePsram(16384);
  if (!input.buffer) {
    setError(ImageRenderError::OutOfMemory);
    LOG_ERR("JPG", "OOM: progressive input buffer");
    return false;
  }
  Budget budget;
  budget.cancellation = cfg.cancellation;
  activeBudget = &budget;
  struct ResetBudget {
    ~ResetBudget() { activeBudget = nullptr; }
  } reset;
  int w = 0, h = 0, channels = 0;
  const bool infoOk = stbi_info_from_callbacks(&callbacks, &input, &w, &h, &channels) != 0;
  const bool exceedsBudget = w > 0 && h > 0 && static_cast<uint64_t>(w) * h > kMaxPixels;
  if (!infoOk || input.failed || w <= 0 || h <= 0 || exceedsBudget) {
    LOG_ERR("JPG", "Progressive image exceeds full-decode limits or has an invalid header: %dx%d", w, h);
    if (cfg.cancellation.isCancelled())
      setError(ImageRenderError::Cancelled);
    else if (budget.oom || exceedsBudget)
      setError(ImageRenderError::OutOfMemory);
    return false;
  }
  if (!input.file.seek(0)) return false;
  input.start = input.end = 0;
  auto* pixels = stbi_load_from_callbacks(&callbacks, &input, &w, &h, &channels, 1);
  struct FreePixels {
    uint8_t* p;
    ~FreePixels() { stbi_image_free(p); }
  } release{pixels};
  if (!pixels || input.failed || cfg.cancellation.isCancelled()) {
    setError(cfg.cancellation.isCancelled() ? ImageRenderError::Cancelled
             : budget.oom                   ? ImageRenderError::OutOfMemory
                                            : ImageRenderError::Failed);
    LOG_ERR("JPG", "Full progressive decode failed (oom=%d, io=%d)", budget.oom, input.failed);
    return false;
  }
  int dw = cfg.maxWidth, dh = cfg.maxHeight;
  if (!cfg.useExactDimensions) {
    const double scale = std::min({1.0, dw > 0 ? double(dw) / w : 1.0, dh > 0 ? double(dh) / h : 1.0});
    dw = static_cast<int>(w * scale);
    dh = static_cast<int>(h * scale);
  }
  if (dw <= 0 || dh <= 0 || cfg.x < 0 || cfg.y < 0 || dw > renderer.getScreenWidth() - cfg.x ||
      dh > renderer.getScreenHeight() - cfg.y)
    return false;
  // Whole-image neighbourhoods avoid MCU-boundary clamps. Keep the user's
  // nearest/bilinear choice; both sample the fully decoded source.
  for (int y = 0; y < dh; ++y) {
    if (cfg.cancellation.isCancelled()) {
      setError(ImageRenderError::Cancelled);
      return false;
    }
    ImageToFramebufferDecoder::yieldDuringDecode(input.yielded);
    if (cfg.cancellation.isCancelled()) {
      setError(ImageRenderError::Cancelled);
      return false;
    }
    const int64_t sy = std::max<int64_t>(0, ((int64_t{2} * y + 1) * h * 256 / dh - 256) / 2);
    const int y0 = std::min(h - 1, static_cast<int>(sy / 256)), y1 = std::min(h - 1, y0 + 1);
    const int fy = sy & 255;
    for (int x = 0; x < dw; ++x) {
      if (!cfg.bilinearScaling) {
        renderer.drawGrayscale16Pixel(
            cfg.x + x, cfg.y + y, pixels[(static_cast<int64_t>(y) * h / dh) * w + static_cast<int64_t>(x) * w / dw]);
        continue;
      }
      const int64_t sx = std::max<int64_t>(0, ((int64_t{2} * x + 1) * w * 256 / dw - 256) / 2);
      const int x0 = std::min(w - 1, static_cast<int>(sx / 256)), x1 = std::min(w - 1, x0 + 1);
      const int fx = sx & 255;
      const int top = pixels[y0 * w + x0] * (256 - fx) + pixels[y0 * w + x1] * fx;
      const int bot = pixels[y1 * w + x0] * (256 - fx) + pixels[y1 * w + x1] * fx;
      const uint8_t gray = static_cast<uint8_t>((top * (256 - fy) + bot * fy + 32768) >> 16);
      renderer.drawGrayscale16Pixel(cfg.x + x, cfg.y + y, gray);
    }
  }
  setError(ImageRenderError::None);
  LOG_DBG("JPG", "Full progressive JPEG %dx%d -> %dx%d (no DC preview)", w, h, dw, dh);
  return true;
}

HalMemory::PsramBuffer decodePlusJpegGrayForBox(const std::string& path, const int boxWidth, const int boxHeight,
                                                const bool fill, int& outWidth, int& outHeight) {
  outWidth = outHeight = 0;
  if (boxWidth <= 0 || boxHeight <= 0 || activeBudget) return {};
  Input input;
  if (!Storage.openFileForRead("JPG", path, input.file)) return {};
  input.buffer = HalMemory::allocatePsram(16384);
  if (!input.buffer) return {};
  // One image for a static screen, decoded while nothing else holds PSRAM: allow most of it.
  // Full coefficients cost about 5.5 bytes a pixel for 4:2:0, so this takes ~2 MP covers.
  Budget budget;
  const size_t free = HalMemory::getPsramHeap().freeBytes;
  budget.limit = free > 2 * kReserve ? std::min<size_t>(free - 2 * kReserve, 13u * 1024 * 1024) : 0;
  activeBudget = &budget;
  struct ResetBudget {
    ~ResetBudget() { activeBudget = nullptr; }
  } reset;
  int w = 0, h = 0, channels = 0;
  if (!stbi_info_from_callbacks(&callbacks, &input, &w, &h, &channels) || input.failed || w <= 0 || h <= 0 ||
      static_cast<uint64_t>(w) * h > 3u * 1024 * 1024 || !input.file.seek(0))
    return {};
  input.start = input.end = 0;
  auto* pixels = stbi_load_from_callbacks(&callbacks, &input, &w, &h, &channels, 1);
  struct FreePixels {
    uint8_t* p;
    ~FreePixels() { stbi_image_free(p); }
  } release{pixels};
  if (!pixels || input.failed) {
    LOG_ERR("JPG", "Card cover full decode failed (%dx%d, oom=%d, io=%d)", w, h, budget.oom, input.failed);
    return {};
  }
  // Source window (fill crops the long side symmetrically) and output size: exactly the box
  // (filled) or its largest fit; a source smaller than that is enlarged bilinearly.
  double srcX = 0, srcY = 0, srcW = w, srcH = h;
  if (fill) {
    const double box = static_cast<double>(boxWidth) / boxHeight;
    if (static_cast<double>(w) / h > box) {
      srcW = h * box;
      srcX = (w - srcW) / 2;
    } else {
      srcH = w / box;
      srcY = (h - srcH) / 2;
    }
  }
  const double scale = std::min({static_cast<double>(boxWidth) / srcW, static_cast<double>(boxHeight) / srcH});
  const int dw = std::max(1, static_cast<int>(srcW * scale + 0.5)), dh = std::max(1, static_cast<int>(srcH * scale + 0.5));
  auto out = HalMemory::allocatePsram(static_cast<size_t>(dw) * dh);
  if (!out) return {};
  const double step = 1.0 / scale;  // source pixels per output pixel
  for (int y = 0; y < dh; ++y) {
    ImageToFramebufferDecoder::yieldDuringDecode(input.yielded);
    uint8_t* row = out.get() + static_cast<size_t>(y) * dw;
    if (step > 1.0) {
      // Area average: every source pixel under the output pixel counts once.
      const int y0 = static_cast<int>(srcY + y * step), y1 = std::max(y0 + 1, std::min(h, static_cast<int>(srcY + (y + 1) * step)));
      for (int x = 0; x < dw; ++x) {
        const int x0 = static_cast<int>(srcX + x * step), x1 = std::max(x0 + 1, std::min(w, static_cast<int>(srcX + (x + 1) * step)));
        uint32_t sum = 0;
        for (int sy = y0; sy < y1; ++sy) {
          const uint8_t* src = pixels + static_cast<size_t>(sy) * w;
          for (int sx = x0; sx < x1; ++sx) sum += src[sx];
        }
        row[x] = static_cast<uint8_t>(sum / static_cast<uint32_t>((y1 - y0) * (x1 - x0)));
      }
    } else {
      // Bilinear when the box is larger than the source window.
      const double fyS = std::max(0.0, srcY + (y + 0.5) * step - 0.5);
      const int y0 = std::min(h - 1, static_cast<int>(fyS)), y1 = std::min(h - 1, y0 + 1);
      const int fy = static_cast<int>((fyS - y0) * 256);
      for (int x = 0; x < dw; ++x) {
        const double fxS = std::max(0.0, srcX + (x + 0.5) * step - 0.5);
        const int x0 = std::min(w - 1, static_cast<int>(fxS)), x1 = std::min(w - 1, x0 + 1);
        const int fx = static_cast<int>((fxS - x0) * 256);
        const int top = pixels[y0 * w + x0] * (256 - fx) + pixels[y0 * w + x1] * fx;
        const int bot = pixels[y1 * w + x0] * (256 - fx) + pixels[y1 * w + x1] * fx;
        row[x] = static_cast<uint8_t>((top * (256 - fy) + bot * fy + 32768) >> 16);
      }
    }
  }
  LOG_INF("JPG", "Card cover %dx%d -> %dx%d (full decode)", w, h, dw, dh);
  outWidth = dw;
  outHeight = dh;
  return out;
}
