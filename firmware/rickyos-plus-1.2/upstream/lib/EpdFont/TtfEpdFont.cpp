#include "TtfEpdFont.h"

#include <cstring>

#if CROSSPOINT_VECTOR_FONTS

#include <Logging.h>
#if RICKYOS_PAINT_PROFILE
#include <Arduino.h>
#endif
#include <MemoryManager.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <algorithm>

namespace {

// Cache growth policy: PsramAlloc ABORTS when fiFontMalloc fails, and vector
// doubling transiently needs old+new blocks, so every cache expansion must be
// an exact, heap-checked reserve. On no-PSRAM boards the block lands in
// internal DRAM and must leave working headroom for the rest of the system.
bool canGrow(const size_t bytes) {
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= bytes) return true;
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= bytes &&
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= bytes + 12 * 1024;
}

// Exact reserve to `need` rounded up to `step`, capped at `ceil`. Returns
// false (leaving the vector untouched) when the heap cannot fund it.
template <typename V>
bool reserveChecked(V& v, const size_t need, const size_t step, const size_t ceil) {
  if (v.capacity() >= need) return true;
  size_t newCap = ((need + step - 1) / step) * step;
  if (newCap > ceil) newCap = ceil;
  if (newCap < need) newCap = need;  // need may legitimately exceed ceil rounding
  if (!canGrow(newCap * sizeof(typename V::value_type))) return false;
  v.reserve(newCap);
  return true;
}
}  // namespace

void TtfEpdFont::addResidentSource(const uint8_t style, const uint8_t* data, const uint32_t len) {
  if (style >= 4 || data == nullptr || len == 0) return;
  Source& s = sources_[style];
  s = Source{};
  s.present = true;
  s.streamed = false;
  s.data = data;
  s.len = len;
}

void TtfEpdFont::addStreamSource(const uint8_t style, const freeink::font::FtFont::ReadFn read, void* ctx,
                                 const unsigned long fileSize) {
  if (style >= 4 || read == nullptr || fileSize == 0) return;
  Source& s = sources_[style];
  s = Source{};
  s.present = true;
  s.streamed = true;
  s.read = read;
  s.ctx = ctx;
  s.fileSize = fileSize;
}

void TtfEpdFont::resolveFaces() {
  // Map each style face onto the best available source. Regular (0) anchors the
  // family; the others prefer an explicit file, then fall back to synthesizing
  // from a related source (wght axis / faux bold for weight, ital axis / oblique
  // for slant — all handled inside FtFont).
  const bool haveB = sources_[Bold].present;
  const bool haveI = sources_[Italic].present;
  const bool haveBI = sources_[BoldItalic].present;

  // regular
  faces_[Regular].srcIndex = Regular;
  faces_[Regular].weight = 400;
  faces_[Regular].wantItalic = false;

  // bold
  faces_[Bold].srcIndex = haveB ? Bold : Regular;
  faces_[Bold].weight = haveB ? 400 : 700;
  faces_[Bold].wantItalic = false;

  // italic
  faces_[Italic].srcIndex = haveI ? Italic : Regular;
  faces_[Italic].weight = 400;
  faces_[Italic].wantItalic = !haveI;  // oblique/axis only when using the roman source

  // bold-italic: dedicated file > bold-of-italic-file > italic-of-bold-file > roman
  Face& bi = faces_[BoldItalic];
  if (haveBI) {
    bi.srcIndex = BoldItalic;
    bi.weight = 400;
    bi.wantItalic = false;
  } else if (haveI) {
    bi.srcIndex = Italic;
    bi.weight = 700;  // wght axis / faux bold on the italic design
    bi.wantItalic = false;
  } else if (haveB) {
    bi.srcIndex = Bold;
    bi.weight = 400;
    bi.wantItalic = true;  // oblique on the bold design
  } else {
    bi.srcIndex = Regular;
    bi.weight = 700;
    bi.wantItalic = true;
  }
}

bool TtfEpdFont::load(const uint16_t pointSize, const bool twoBit, const size_t glyphCacheBytes,
                      const uint16_t maxGlyphs, const bool fourBit) {
  loaded_ = false;
  if (!sources_[Regular].present) return false;
  // CrossPoint speaks point-size-at-150-DPI (matching the .cpfont converter's
  // FT_Set_Char_Size(size, size, 150, 150)); FreeInkFont speaks pixels. Convert
  // so vector fonts match the on-glyph size and metrics of the bitmap fonts:
  //   ppem = pointSize * 150 / 72, kept in 26.6 so the fractional part survives
  // (glyphs raster and advance at the exact ppem; the per-face sizePx is the
  // rounded form for the integer-pixel Font API).
  size26_6_ = (static_cast<uint32_t>(pointSize) * 150u * 64u + 36u) / 72u;
  const uint16_t sizePx = static_cast<uint16_t>((size26_6_ + 32u) >> 6);
  resolveFaces();
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    f.owner = this;
    f.twoBit = twoBit;
    f.fourBit = fourBit;
    f.sizePx = sizePx;
    f.cap = glyphCacheBytes;
    f.maxGlyphs = maxGlyphs;
    f.inited = false;
    f.ready = false;
    // Flush the glyph/kern caches, not just the arena cursor: faultGlyph
    // consults the lookup tables before anything else, so entries rasterized
    // at a previous size would serve stale metrics — and their bitmaps get
    // overwritten as new-size glyphs refill the arena from offset 0.
    flushFace(f);
    f.metricCps.clear();  // metrics are per size
    f.metrics.clear();
    f.ligPairCount = 0;                  // re-resolved in initFace; stale pairs must not leak
    for (uint32_t& g : f.ligGid) g = 0;  // across a reload with new sources
  }
  initFace(faces_[0]);  // regular eagerly: validates the font + gives metrics
  if (!faces_[0].ready) return false;
  hmtx_ = HorizontalMetrics{};
  if (lazyMetrics_ && !loadHorizontalMetrics()) hmtx_ = HorizontalMetrics{};
  for (int i = 1; i < 4; ++i) setupFace(faces_[i]);  // handlers + placeholder metrics (regular's)
  loaded_ = true;
  return true;
}

void TtfEpdFont::initFace(Face& f) {
  if (f.inited) return;
  f.inited = true;
  const Source& s = sources_[f.srcIndex];
  if (!s.present) {
    f.ready = false;
  } else if (s.streamed) {
    f.ready = f.ft.initStream(s.read, s.ctx, s.fileSize, f.sizePx, f.weight, f.wantItalic);
  } else {
    f.ready = f.ft.init(s.data, s.len, f.sizePx, f.weight, f.wantItalic);
  }
  if (f.ready) {
    // Full auto-hinting + stem darkening pair with the BW-aware quantizer in
    // faultGlyph: the 50% ink threshold needs stems snapped to full-coverage
    // pixels or strokes break up / vary per letter on BW page turns (see
    // platformio.ini's FREEINK_FONT_ENABLE_AUTOHINT note). Auto (both axes)
    // rather than Light (vertical only): Light leaves stem WIDTHS fractional,
    // so the ink threshold prints the same-width stem as 1px or 2px depending
    // on each glyph's horizontal phase — visibly uneven letter weights. Auto
    // equalizes stem widths across the face. Darkening nudges borderline-thin
    // strokes over the threshold.
    freeink::font::FtFont::RenderOptions ro;
    ro.hinting = freeink::font::FtFont::HintingMode::Auto;
    ro.stemDarkening = true;
    if (!f.ft.setRenderOptions(ro)) {
      LOG_ERR("TTF", "Auto hinting unavailable (FREEINK_FONT_ENABLE_AUTOHINT not compiled)");
    }
    // GPOS kerning for RESIDENT faces only (they borrow a view into the font
    // bytes — free). Unlike GSUB, GPOS must stay resident for render-time
    // pair queries, and a STREAMED face would need an owned DRAM copy exactly
    // when the C3 is poorest (a font big enough to stream, i.e. CJK, where
    // Latin pair kerning barely matters). Budget 0 = streamed faces skip it.
    f.ft.setGposByteBudget(0);
    resolveLigatures(f);
  }
  // Caches are NOT pre-reserved: the byte arena (f.bmp) and the glyph tables
  // grow on demand in faultGlyph and converge on the book's page needs (see the
  // header's memory note). f.cap is only the hard ceiling that triggers a flush.
  setupFace(f);
}

void TtfEpdFont::resolveLigatures(Face& f) {
  // One GSUB pass per face init: resolve the standard Latin ligatures to glyph
  // IDs, then drop the table — nothing else queries GSUB, so a streamed face
  // never keeps its (budgeted) table copy resident.
  static constexpr uint32_t kComps[5][3] = {
      {'f', 'f', 0}, {'f', 'i', 0}, {'f', 'l', 0}, {'f', 'f', 'i'}, {'f', 'f', 'l'}};
  f.ft.setGsubByteBudget(48 * 1024);  // C3 DRAM discipline: oversized GSUB → no ligatures, not OOM
  for (int i = 0; i < 5; ++i) f.ligGid[i] = f.ft.ligatureGlyphId(kComps[i], kComps[i][2] ? 3 : 2);
  f.ft.releaseLigatureTable();

  // Pair table for EpdFont::applyLigatures(), keyed on the U+FB00–FB04
  // presentation codepoints. A ligature is usable if GSUB named a glyph or the
  // cmap maps the presentation codepoint directly. Entries are appended in
  // ascending key order (the ff/fi/fl keys sort below the FB00-chained ones).
  auto avail = [&f](const int i) { return f.ligGid[i] != 0 || f.ft.hasGlyph(0xFB00u + i); };
  f.ligPairCount = 0;
  auto add = [&f](const uint32_t left, const uint32_t right, const uint32_t out) {
    f.ligPairs[f.ligPairCount++] = EpdLigaturePair{(left << 16) | right, out};
  };
  if (avail(0)) add('f', 'f', 0xFB00);
  if (avail(1)) add('f', 'i', 0xFB01);
  if (avail(2)) add('f', 'l', 0xFB02);
  if (avail(0)) {  // ffi/ffl chain through the ff result as the new left
    if (avail(3)) add(0xFB00, 'i', 0xFB03);
    if (avail(4)) add(0xFB00, 'l', 0xFB04);
  }
}

void TtfEpdFont::setupFace(Face& f) {
  // Metrics from this face once live, else borrow the (always-live) regular
  // face's — same font/size, so a fine placeholder until this face is faulted.
  freeink::font::FtFont& src = f.ready ? f.ft : faces_[0].ft;
  // Fractional-ppem line metrics, rounded once here rather than per-call.
  freeink::font::FtFont::LineMetrics lm;
  int ascent, lineHeight;
  if (src.lineMetrics26_6(f.owner->size26_6_, lm)) {
    ascent = static_cast<int>((lm.ascender26_6 + 32) >> 6);
    lineHeight = static_cast<int>((lm.height26_6 + 32) >> 6);
  } else {
    ascent = src.ascent(f.sizePx);
    lineHeight = src.lineHeight(f.sizePx);
  }
  f.data = EpdFontData{};
  f.data.advanceY = static_cast<uint8_t>(lineHeight > 255 ? 255 : (lineHeight < 0 ? 0 : lineHeight));
  f.data.ascender = ascent;
  f.data.descender = lineHeight - ascent > 0 ? lineHeight - ascent : 0;
  f.data.is2Bit = f.twoBit;
  f.data.is4Bit = f.fourBit;
  f.data.glyphMissHandler = &TtfEpdFont::missThunk;
  f.data.glyphMissCtx = &f;
  f.data.coverageHandler = &TtfEpdFont::coverageThunk;
  f.data.vectorBitmapHandler = &TtfEpdFont::bitmapThunk;
  // GSUB-derived ligature pairs (resolveLigatures). Empty until this face
  // inits — a lazy style renders its first pass ligature-free, then picks
  // them up once its glyphs fault the face in.
  f.data.ligaturePairs = f.ligPairCount ? f.ligPairs : nullptr;
  f.data.ligaturePairCount = f.ligPairCount;
  f.data.kernHandler = &TtfEpdFont::kernThunk;
}

void TtfEpdFont::flushFace(Face& f) {
  f.glyphs.clear();
  f.cps.clear();
  f.slot.clear();
  f.gids.clear();
  f.kernKeys.clear();
  f.kernVals.clear();
  f.used = 0;
}

void TtfEpdFont::clearCache() {
  // Glyphs now persist across pages: records are metrics only, bitmaps are drawn on
  // demand and the arena evicts itself when full (rasterizeInto). Dropping them every
  // page made each page re-rasterize every character it shared with the last one.
  // Memory pressure still frees everything through releaseResidentCaches().
}

void TtfEpdFont::releaseResidentCaches() {
  if (evictionLocked_) return;  // mid-fault: this font's faces are live
#if RICKYOS_PAINT_PROFILE
  ++counters().releases;
  LOG_INF("TTF", "release resident caches (free=%u max=%u)", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
#endif
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    // Actually RELEASE the caches (swap-with-empty frees capacity; clear() alone
    // would not).
    flushFace(f);
    freeink::font::PsramVector<uint8_t>().swap(f.bmp);
    freeink::font::PsramVector<EpdGlyph>().swap(f.glyphs);
    freeink::font::PsramVector<uint32_t>().swap(f.cps);
    freeink::font::PsramVector<uint16_t>().swap(f.slot);
    freeink::font::PsramVector<freeink::font::FtFont::GlyphId>().swap(f.gids);
    freeink::font::PsramVector<uint32_t>().swap(f.metricCps);
    freeink::font::PsramVector<GlyphMetricsEntry>().swap(f.metrics);
    freeink::font::PsramVector<uint64_t>().swap(f.kernKeys);
    freeink::font::PsramVector<int8_t>().swap(f.kernVals);
    // Keep the regular face's FreeType face live so coverage()/metrics still
    // answer without a reload (mirrors SD keeping its interval table resident).
    // Shed the lazy bold/italic/bold-italic faces entirely; they re-init on the
    // next glyph fault for that style.
    if (i == 0) continue;
    if (f.inited) {
      f.ft.deinit();
      f.inited = false;
      f.ready = false;
    }
  }
}

const EpdGlyph* TtfEpdFont::faultGlyph(Face& f, const uint32_t cp) {
  if (!f.inited) initFace(f);
  if (!f.ready) return nullptr;
  {
    const auto it = std::lower_bound(f.cps.begin(), f.cps.end(), cp);
    if (it != f.cps.end() && *it == cp) return &f.glyphs[f.slot[static_cast<size_t>(it - f.cps.begin())]];
  }

  // Resolve to a glyph ID: cmap first, then the GSUB result for the ligature
  // presentation codepoints (whose glyphs commonly have no cmap entry at all).
  freeink::font::FtFont::GlyphId gid = f.ft.glyphId(cp);
  if (gid == 0 && cp >= 0xFB00u && cp <= 0xFB04u) gid = f.ligGid[cp - 0xFB00u];
  if (gid == 0) return nullptr;

  // Layout only needs advances and boxes, so a miss records metrics and nothing else;
  // the bitmap is rasterized the first time the glyph is drawn (rasterizeInto, via
  // bitmapThunk). Rasterizing every glyph a chapter build measured made a CJK chapter
  // take most of a minute, and filled the arena with glyphs no page ever showed.
  // Metrics persist per face (metricsFor), across page flushes.
  const uint32_t size26_6 = f.owner->size26_6_;
  const GlyphMetricsEntry* m = metricsFor(f, cp, gid, size26_6);
  if (m == nullptr) return nullptr;

  if (f.glyphs.size() >= f.maxGlyphs) flushFace(f);

  // All growth below is exact and heap-checked (see reserveChecked). On a failed
  // grow, ask the memory manager to evict rebuildable caches (render glyph cache,
  // other fonts' arenas -- evictionLocked_ keeps THIS font's faces alive) and retry
  // once; then the miss is uncached (the caller skips the glyph this pass).
  static constexpr size_t kTableStep = 64;
  const auto growTables = [&]() {
    return reserveChecked(f.glyphs, f.glyphs.size() + 1, kTableStep, f.maxGlyphs) &&
           reserveChecked(f.cps, f.cps.size() + 1, kTableStep, f.maxGlyphs) &&
           reserveChecked(f.slot, f.slot.size() + 1, kTableStep, f.maxGlyphs) &&
           reserveChecked(f.gids, f.gids.size() + 1, kTableStep, f.maxGlyphs);
  };
  if (!growTables()) {
    evictionLocked_ = true;
    freeink::MemoryManager::instance().ensureFree(16 * 1024);
    evictionLocked_ = false;
    if (!growTables()) {
      // The tables cannot grow: start them over within the capacity they have, as at
      // maxGlyphs, rather than leaving every new character of the book unresolved.
      if (f.glyphs.empty()) return nullptr;
      LOG_INF("TTF", "glyph table stuck at %u, flushing", static_cast<unsigned>(f.glyphs.size()));
      flushFace(f);
      if (!growTables()) return nullptr;
    }
  }

  EpdGlyph eg{};
  eg.advanceX = m->advance12_4;
  eg.width = m->width;
  eg.height = m->height;
  eg.left = m->left;
  eg.top = m->top;
  eg.dataOffset = m->boxPending ? kPendingBox : 0;
  eg.dataLength = 0;  // not rasterized yet (or blank when width/height are 0)

  f.glyphs.push_back(eg);
  f.gids.push_back(m->gid);
  const uint16_t newIdx = static_cast<uint16_t>(f.glyphs.size() - 1);
  const auto it = std::lower_bound(f.cps.begin(), f.cps.end(), cp);
  const size_t pos = static_cast<size_t>(it - f.cps.begin());
  f.cps.insert(it, cp);
  f.slot.insert(f.slot.begin() + pos, newIdx);
  return &f.glyphs[newIdx];
}

bool TtfEpdFont::readSource(const Source& s, const uint32_t offset, uint8_t* out, const uint32_t count) const {
  if (!s.present) return false;
  if (!s.streamed) {
    if (s.data == nullptr || offset > s.len || count > s.len - offset) return false;
    memcpy(out, s.data + offset, count);
    return true;
  }
  return s.read != nullptr && s.read(s.ctx, offset, out, count) == count;
}

// Reads unitsPerEm (head), numberOfHMetrics (hhea) and the advance column of hmtx from
// the regular source. Variable fonts (fvar) are skipped: their advances move with the
// axes. A collection (ttcf) uses its first face, as FreeType does.
bool TtfEpdFont::loadHorizontalMetrics() {
  const Source& s = sources_[Regular];
  const auto be16 = [](const uint8_t* p) { return static_cast<uint32_t>(p[0] << 8 | p[1]); };
  const auto be32 = [](const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           p[3];
  };
  uint8_t header[16];
  if (!readSource(s, 0, header, sizeof(header))) return false;
  uint32_t base = 0;
  if (memcmp(header, "ttcf", 4) == 0) {
    base = be32(header + 12);
    if (!readSource(s, base, header, 12)) return false;
  }
  const uint32_t tables = be16(header + 4);
  if (tables == 0 || tables > 64) return false;
  uint32_t head = 0, hhea = 0, hmtx = 0, hmtxLen = 0;
  for (uint32_t i = 0; i < tables; ++i) {
    uint8_t record[16];
    if (!readSource(s, base + 12 + i * 16, record, sizeof(record))) return false;
    if (memcmp(record, "fvar", 4) == 0) return false;
    if (memcmp(record, "head", 4) == 0) head = be32(record + 8);
    if (memcmp(record, "hhea", 4) == 0) hhea = be32(record + 8);
    if (memcmp(record, "hmtx", 4) == 0) {
      hmtx = be32(record + 8);
      hmtxLen = be32(record + 12);
    }
  }
  if (head == 0 || hhea == 0 || hmtx == 0) return false;
  uint8_t buf[2];
  if (!readSource(s, head + 18, buf, 2)) return false;
  hmtx_.unitsPerEm = static_cast<uint16_t>(be16(buf));
  if (!readSource(s, hhea + 34, buf, 2)) return false;
  const uint32_t count = be16(buf);
  if (hmtx_.unitsPerEm == 0 || count == 0 || count * 4 > hmtxLen) return false;
  if (!reserveChecked(hmtx_.advances, count, count, count)) return false;
  hmtx_.advances.resize(count);
  // longHorMetric = {uint16 advanceWidth, int16 lsb}; read in chunks, keep the advances.
  uint8_t chunk[1024];
  for (uint32_t done = 0; done < count;) {
    const uint32_t n = std::min<uint32_t>(count - done, sizeof(chunk) / 4);
    if (!readSource(s, hmtx + done * 4, chunk, n * 4)) return false;
    for (uint32_t i = 0; i < n; ++i) hmtx_.advances[done + i] = static_cast<uint16_t>(be16(chunk + i * 4));
    done += n;
  }
  hmtx_.ok = true;
  LOG_DBG("TTF", "hmtx: %u advances, %u units/em", static_cast<unsigned>(count),
          static_cast<unsigned>(hmtx_.unitsPerEm));
  return true;
}

bool TtfEpdFont::usesHmtx(const Face& f) const {
  return hmtx_.ok && &f == &faces_[Regular] && f.srcIndex == Regular && f.weight == 400 && !f.wantItalic;
}

const TtfEpdFont::GlyphMetricsEntry* TtfEpdFont::metricsFor(Face& f, const uint32_t cp,
                                                            const freeink::font::FtFont::GlyphId gid,
                                                            const uint32_t size26_6) {
  const auto it = std::lower_bound(f.metricCps.begin(), f.metricCps.end(), cp);
  const size_t pos = static_cast<size_t>(it - f.metricCps.begin());
  if (it != f.metricCps.end() && *it == cp) return &f.metrics[pos];

  GlyphMetricsEntry entry{};
  entry.gid = gid;
  freeink::font::FtFont::GlyphMetrics gm;
  if (usesHmtx(f)) {
    // The linear advance FreeType would report, straight from hmtx: no outline read.
    const auto& advances = hmtx_.advances;
    const uint32_t units = advances[gid < advances.size() ? gid : advances.size() - 1];
    const uint32_t upm = hmtx_.unitsPerEm;
    const uint32_t adv12_4 = (units * size26_6 + 2 * upm) / (4 * upm);
    entry.advance12_4 = static_cast<uint16_t>(adv12_4 > 0xFFFF ? 0xFFFF : adv12_4);
    entry.boxPending = true;
  } else if (f.ft.metricsGlyph26_6(gid, size26_6, gm)) {
    const int32_t adv12_4 = (gm.advance26_6 + 2) >> 2;
    entry.advance12_4 = static_cast<uint16_t>(adv12_4 < 0 ? 0 : (adv12_4 > 0xFFFF ? 0xFFFF : adv12_4));
    entry.width = static_cast<uint8_t>(gm.width > 255 ? 255 : gm.width);
    entry.height = static_cast<uint8_t>(gm.height > 255 ? 255 : gm.height);
    entry.left = gm.left;
    entry.top = gm.top;
  } else {
    // No outline metrics (bitmap-only strike, odd glyph): take them from one raster.
    const freeink::font::GlyphBitmap* g = f.ft.rasterizeGlyph26_6(gid, size26_6);
    if (g == nullptr) return nullptr;
    entry.advance12_4 = static_cast<uint16_t>(g->advance << 4);
    entry.width = static_cast<uint8_t>(g->width > 255 ? 255 : g->width);
    entry.height = static_cast<uint8_t>(g->height > 255 ? 255 : g->height);
    entry.left = g->xoff;
    entry.top = static_cast<int16_t>(-g->yoff);
  }
  // Bounded: past the cap a metric is used once and not kept (a pathological book).
  static constexpr size_t kMaxMetrics = 16384;
  if (f.metricCps.size() >= kMaxMetrics || !reserveChecked(f.metricCps, f.metricCps.size() + 1, 256, kMaxMetrics) ||
      !reserveChecked(f.metrics, f.metrics.size() + 1, 256, kMaxMetrics)) {
    f.scratchMetrics = entry;
    return &f.scratchMetrics;
  }
  f.metricCps.insert(f.metricCps.begin() + pos, cp);
  f.metrics.insert(f.metrics.begin() + pos, entry);
  return &f.metrics[pos];
}

const uint8_t* TtfEpdFont::rasterizeInto(Face& f, EpdGlyph& glyph) {
  const size_t index = static_cast<size_t>(&glyph - f.glyphs.data());
  if (index >= f.glyphs.size() || index >= f.gids.size()) return nullptr;
#if RICKYOS_PAINT_PROFILE
  const uint32_t rasterStart = micros();
  const freeink::font::GlyphBitmap* g = f.ft.rasterizeGlyph26_6(f.gids[index], size26_6_);
  ++counters().rasterized;
  counters().rasterUs += micros() - rasterStart;
#else
  const freeink::font::GlyphBitmap* g = f.ft.rasterizeGlyph26_6(f.gids[index], size26_6_);
#endif
  if (glyph.dataOffset == kPendingBox) {
    // Advance came from hmtx; the raster supplies the box.
    glyph.dataOffset = 0;
    if (g == nullptr || g->pixels == nullptr || g->width == 0 || g->height == 0) {
      glyph.width = 0;
      glyph.height = 0;
      return nullptr;  // blank glyph (space)
    }
    glyph.width = static_cast<uint8_t>(g->width > 255 ? 255 : g->width);
    glyph.height = static_cast<uint8_t>(g->height > 255 ? 255 : g->height);
    glyph.left = g->xoff;
    glyph.top = static_cast<int16_t>(-g->yoff);
  }
  if (g == nullptr || g->pixels == nullptr) return nullptr;
  const uint32_t w = glyph.width;
  const uint32_t h = glyph.height;
  const uint32_t px = w * h;
  const size_t bytes = f.fourBit ? (px + 1) / 2 : (f.twoBit ? (px + 3) / 4 : (px + 7) / 8);
  if (bytes == 0 || bytes > f.cap) return nullptr;
  // Arena full: drop every bitmap but keep the glyph records (and the metrics), so
  // nothing layout holds goes stale; glyphs re-rasterize as they are drawn again.
  const auto recycleArena = [&f]() {
    for (EpdGlyph& e : f.glyphs) e.dataLength = 0;
    f.used = 0;
  };
  if (f.used + bytes > f.cap) recycleArena();
  // Grow in large steps: every growth copies the whole arena into a new block, so
  // 4 KB steps meant hundreds of near-1 MB copies and, once PSRAM was fragmented,
  // a growth that could never succeed.
  static constexpr size_t kArenaStep = 64 * 1024;
  if (!reserveChecked(f.bmp, f.used + bytes, kArenaStep, f.cap)) {
    evictionLocked_ = true;
    freeink::MemoryManager::instance().ensureFree(16 * 1024);
    evictionLocked_ = false;
    if (!reserveChecked(f.bmp, f.used + bytes, kArenaStep, f.cap)) {
      // The arena cannot grow any further: treat what it already has as the cap and
      // recycle it. Returning here instead left every glyph not yet drawn blank for
      // the rest of the book (the arena never reached f.cap, so it never recycled).
      if (f.bmp.capacity() < bytes) {
        LOG_ERR("TTF", "glyph arena cannot hold %u B (capacity %u)", static_cast<unsigned>(bytes),
                static_cast<unsigned>(f.bmp.capacity()));
        return nullptr;
      }
      LOG_INF("TTF", "glyph arena stuck at %u KB, recycling", static_cast<unsigned>(f.bmp.capacity() / 1024));
      recycleArena();
    }
  }
  if (f.bmp.size() < f.used + bytes) f.bmp.resize(f.used + bytes, 0);
  uint8_t* dst = f.bmp.data() + f.used;
  for (size_t i = 0; i < bytes; ++i) dst[i] = 0;
  // Place the raster in the metrics box the layout used; FreeType's bitmap box matches
  // it, and any difference is a clipped edge pixel, never a shifted glyph.
  const int dx = g->xoff - glyph.left;
  const int dy = glyph.top - (-g->yoff);
  for (int ry = 0; ry < g->height; ++ry) {
    const int ty = ry + dy;
    if (ty < 0 || ty >= static_cast<int>(h)) continue;
    for (int rx = 0; rx < g->width; ++rx) {
      const int tx = rx + dx;
      if (tx < 0 || tx >= static_cast<int>(w)) continue;
      const uint8_t a = g->pixels[ry * g->width + rx];
      if (a == 0) continue;
      const uint32_t i = static_cast<uint32_t>(ty) * w + static_cast<uint32_t>(tx);
      if (f.fourBit) {
        // High nibble first; truncation preserves the original two-bit thresholds on fallback.
        dst[i >> 1] |= static_cast<uint8_t>((a >> 4) << ((1 - (i & 1)) * 4));
      } else if (f.twoBit) {
        const uint8_t v = a < 64 ? 0 : (a < 128 ? 1 : (a < 192 ? 2 : 3));
        dst[i >> 2] |= static_cast<uint8_t>(v << ((3 - (i & 3)) * 2));
      } else if (a >= 128) {
        dst[i >> 3] |= static_cast<uint8_t>(1u << (7 - (i & 7)));
      }
    }
  }
  glyph.dataOffset = static_cast<uint32_t>(f.used);
  glyph.dataLength = static_cast<uint16_t>(bytes);
  f.used += bytes;
  return dst;
}

int8_t TtfEpdFont::faultKern(Face& f, const uint32_t leftCp, const uint32_t rightCp) {
  if (leftCp == 0 || rightCp == 0) return 0;
  if (!f.inited) initFace(f);
  if (!f.ready) return 0;
  const uint64_t key = (uint64_t(leftCp) << 32) | rightCp;
  {
    const auto it = std::lower_bound(f.kernKeys.begin(), f.kernKeys.end(), key);
    if (it != f.kernKeys.end() && *it == key) return f.kernVals[static_cast<size_t>(it - f.kernKeys.begin())];
  }

  // 26.6 → 4.4 (round-half-away), clamped to the int8 contract (±8px, far
  // beyond any real kern at reader sizes).
  const int32_t k26 = f.ft.kerning26_6(leftCp, rightCp, f.owner->size26_6_);
  int32_t k4 = (k26 + (k26 < 0 ? -2 : 2)) / 4;
  if (k4 < -128) k4 = -128;
  if (k4 > 127) k4 = 127;

  // Text uses few distinct pairs; the cap only guards against pathological
  // content churning the cache without bound.
  static constexpr size_t kKernCap = 512;
  if (f.kernKeys.size() >= kKernCap) {
    f.kernKeys.clear();
    f.kernVals.clear();
  }
  // One-time exact reserve: with capacity pinned at the cap, the sorted
  // inserts below never touch the allocator. Uncached result if the heap
  // cannot fund the cache at all.
  if (!reserveChecked(f.kernKeys, kKernCap, kKernCap, kKernCap) ||
      !reserveChecked(f.kernVals, kKernCap, kKernCap, kKernCap)) {
    return static_cast<int8_t>(k4);
  }
  const auto it = std::lower_bound(f.kernKeys.begin(), f.kernKeys.end(), key);
  const size_t pos = static_cast<size_t>(it - f.kernKeys.begin());
  f.kernKeys.insert(it, key);
  f.kernVals.insert(f.kernVals.begin() + pos, static_cast<int8_t>(k4));
  return static_cast<int8_t>(k4);
}

const EpdGlyph* TtfEpdFont::missThunk(void* ctx, const uint32_t codepoint) {
  Face* f = static_cast<Face*>(ctx);
  return f->owner->faultGlyph(*f, codepoint);
}
int8_t TtfEpdFont::kernThunk(void* ctx, const uint32_t leftCp, const uint32_t rightCp) {
  Face* f = static_cast<Face*>(ctx);
  return f->owner->faultKern(*f, leftCp, rightCp);
}
const uint8_t* TtfEpdFont::bitmapThunk(void* ctx, const EpdGlyph* glyph) {
  if (glyph == nullptr) return nullptr;
  Face* f = static_cast<Face*>(ctx);
  if (glyph->dataLength != 0) return f->bmp.data() + glyph->dataOffset;
  if ((glyph->width == 0 || glyph->height == 0) && glyph->dataOffset != kPendingBox) return nullptr;  // blank
  // First draw of a glyph layout measured: rasterize it now. The record lives in this
  // face's table, which the renderer only reads, so updating its bitmap fields is safe.
  return f->owner->rasterizeInto(*f, const_cast<EpdGlyph&>(*glyph));
}
bool TtfEpdFont::coverageThunk(void* ctx, const uint32_t codepoint) {
  // All styles share one file → coverage comes from the always-live regular face.
  Face& reg = static_cast<Face*>(ctx)->owner->faces_[0];
  if (reg.ft.hasGlyph(codepoint)) return true;
  // Ligature presentation codepoints resolvable through GSUB despite no cmap
  // entry (some EPUBs carry literal U+FB01/U+FB02 in their text).
  return codepoint >= 0xFB00u && codepoint <= 0xFB04u && reg.ligGid[codepoint - 0xFB00u] != 0;
}

EpdFontFamily TtfEpdFont::family() const {
  return EpdFontFamily(&faces_[0].font, &faces_[1].font, &faces_[2].font, &faces_[3].font);
}

bool TtfEpdFont::build(const char* utf8) {
  if (!loaded_) return false;
  flushFace(faces_[0]);
  return addCoverage(utf8);
}
TtfEpdFont::Prerasterize TtfEpdFont::prerasterize(const uint32_t cp) {
  if (!loaded_) return Prerasterize::Full;
  Face& f = faces_[0];
  if (!f.inited) initFace(f);
  if (!f.ready) return Prerasterize::Full;
  static constexpr size_t kArenaMargin = 64 * 1024;
  if (f.glyphs.size() + f.maxGlyphs / 10 >= f.maxGlyphs || f.used + kArenaMargin >= f.cap) return Prerasterize::Full;
#if defined(ESP_PLATFORM)
  // Growing the arena takes a 64 KB step of PSRAM; a short heap would answer with an
  // eviction (MemoryManager::ensureFree) of the page cache or other fonts.
  if (f.used + 4096 > f.bmp.capacity() && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 2 * 1024 * 1024) {
    return Prerasterize::Full;
  }
#endif
  const EpdGlyph* glyph = faultGlyph(f, cp);
  if (glyph == nullptr || glyph->dataLength != 0) return Prerasterize::Skipped;  // absent or already drawn
  if (glyph->dataOffset != kPendingBox && (glyph->width == 0 || glyph->height == 0)) return Prerasterize::Skipped;
  rasterizeInto(f, const_cast<EpdGlyph&>(*glyph));
  return Prerasterize::Done;
}

bool TtfEpdFont::addCoverage(const char* utf8) {
  if (!loaded_ || utf8 == nullptr) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p != '\0') faultGlyph(faces_[0], utf8NextCodepoint(&p));
  return true;
}

#endif  // CROSSPOINT_VECTOR_FONTS
