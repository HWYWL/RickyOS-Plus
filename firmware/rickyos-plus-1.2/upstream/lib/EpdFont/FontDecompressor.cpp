#include "FontDecompressor.h"

#include <Arduino.h>
#include <FontAlloc.h>  // PSRAM-preferring font allocator (fiFontMalloc/Free)
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdlib>

// Decompressed-glyph page slots and the hot-group buffers are placed in PSRAM
// when the board has it (fiFontMalloc), falling back to the internal heap
// otherwise — so the built-in compressed fonts get the same lift as the SD and
// vector paths. fiFontFree releases either region.

FontDecompressor::~FontDecompressor() { deinit(); }

bool FontDecompressor::init() {
  clearCache();
  return true;
}

void FontDecompressor::deinit() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::clearCache() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::freePageBuffer() {
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    fiFontFree(pageSlots[s].buffer);
    fiFontFree(pageSlots[s].glyphs);
    pageSlots[s] = {};
  }
  pageSlotCount = 0;
}

void FontDecompressor::freeHotGroup() {
  for (auto& slot : hotGroups) {
    fiFontFree(slot.data);
    fiFontFree(slot.offsets);
    fiFontFree(slot.members);
    slot = {};
  }
  hotGroupClock = 0;
  fiFontFree(hotGlyphBuf);
  hotGlyphBuf = nullptr;
  hotGlyphBufCapacity = 0;
}

template <typename T>
bool FontDecompressor::ensureCapacity(T*& buf, uint32_t& capacity, uint32_t needed) {
  if (capacity >= needed) return true;
  // Grow-only, free-then-malloc: every caller fully rewrites the buffer after a grow, so the
  // old contents are dead -- freeing first gives the allocator its best shot on a tight heap.
  fiFontFree(buf);
  buf = static_cast<T*>(fiFontMalloc(needed * sizeof(T)));  // owned by FontDecompressor, freed in freeHotGroup()
  capacity = buf ? needed : 0;
  return buf != nullptr;
}

// Explicit instantiations: this is a private helper and both element types are used here.
template bool FontDecompressor::ensureCapacity<uint8_t>(uint8_t*&, uint32_t&, uint32_t);
template bool FontDecompressor::ensureCapacity<uint32_t>(uint32_t*&, uint32_t&, uint32_t);

uint32_t FontDecompressor::totalGlyphs(const EpdFontData* fontData) {
  if (fontData->intervalCount == 0) return 0;
  const auto& lastInterval = fontData->intervals[fontData->intervalCount - 1];
  return lastInterval.offset + (lastInterval.last - lastInterval.first + 1);
}

uint16_t FontDecompressor::getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex) {
  // O(1) path for frequency-grouped fonts with glyphToGroup mapping
  if (fontData->glyphToGroup != nullptr) {
    return fontData->glyphToGroup[glyphIndex];
  }

  // Contiguous-group fonts: groups are stored in glyph order, so binary-search the last
  // group starting at or before the glyph (a CJK face has hundreds of groups).
  uint16_t lo = 0;
  uint16_t hi = fontData->groupCount;
  while (lo < hi) {
    const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
    if (fontData->groups[mid].firstGlyphIndex <= glyphIndex)
      lo = static_cast<uint16_t>(mid + 1);
    else
      hi = mid;
  }
  if (lo > 0) {
    const EpdFontGroup& group = fontData->groups[lo - 1];
    if (glyphIndex < group.firstGlyphIndex + group.glyphCount) return static_cast<uint16_t>(lo - 1);
  }
  return fontData->groupCount;  // sentinel = not found
}

bool FontDecompressor::decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf,
                                       uint32_t outSize) {
  const EpdFontGroup& group = fontData->groups[groupIndex];

  const uint32_t tDecomp = millis();
  inflateReader.init(false);
  inflateReader.setSource(&fontData->bitmap[group.compressedOffset], group.compressedSize);
  if (!inflateReader.read(outBuf, outSize)) {
    stats.decompressTimeMs += millis() - tDecomp;
    LOG_ERR("FDC", "Decompression failed for group %u", groupIndex);
    return false;
  }
  stats.decompressTimeMs += millis() - tDecomp;
  return true;
}

// --- Byte-aligned helpers ---

uint32_t FontDecompressor::getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex) {
  uint32_t offset = 0;

  auto accumGlyph = [&](const EpdGlyph& g) {
    if (g.width > 0 && g.height > 0) {
      offset += ((g.width + (fontData->is4Bit ? 1 : 3)) / (fontData->is4Bit ? 2 : 4)) * g.height;
    }
  };

  if (fontData->glyphToGroup) {
    // Frequency-grouped: scan glyphs before glyphIndex that belong to this group
    for (uint32_t i = 0; i < glyphIndex; i++) {
      if (fontData->glyphToGroup[i] == groupIndex) {
        accumGlyph(fontData->glyph[i]);
      }
    }
  } else {
    // Contiguous-group: sum aligned sizes of preceding glyphs in the group
    const EpdFontGroup& group = fontData->groups[groupIndex];
    for (uint32_t i = group.firstGlyphIndex; i < glyphIndex; i++) {
      accumGlyph(fontData->glyph[i]);
    }
  }

  return offset;
}

void FontDecompressor::compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width,
                                          uint8_t height, bool is4Bit) {
  if (width == 0 || height == 0) return;
  const uint8_t bits = is4Bit ? 4 : 2;
  const uint8_t perByte = 8 / bits;
  const uint8_t mask = (1u << bits) - 1;
  const uint32_t rowStride = (width + perByte - 1) / perByte;
  if (width % perByte == 0) {
    memcpy(packedDst, alignedSrc, rowStride * height);
    return;
  }
  uint8_t outByte = 0, outBits = 0;
  uint32_t writeIdx = 0;
  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++) {
      outByte = (outByte << bits) | ((alignedSrc[y * rowStride + x / perByte] >> ((perByte - 1 - (x % perByte)) * bits)) & mask);
      outBits += bits;
      if (outBits == 8) {
        packedDst[writeIdx++] = outByte;
        outByte = 0;
        outBits = 0;
      }
    }
  }
  if (outBits > 0) packedDst[writeIdx] = outByte << (8 - outBits);
}

// --- getBitmap: page buffer → hot group → decompress ---

const uint8_t* FontDecompressor::getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex) {
  const uint32_t tStart = micros();
  stats.getBitmapCalls++;

  if (!fontData->groups || fontData->groupCount == 0) {
    stats.getBitmapTimeUs += micros() - tStart;
    return &fontData->bitmap[glyph->dataOffset];
  }

  // Check page buffer slots (populated by prewarmCache — one slot per font style)
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    const auto& slot = pageSlots[s];
    if (slot.fontData != fontData || slot.glyphCount == 0) continue;

    int left = 0, right = slot.glyphCount - 1;
    while (left <= right) {
      int mid = left + (right - left) / 2;
      if (slot.glyphs[mid].glyphIndex == glyphIndex) {
        if (slot.glyphs[mid].bufferOffset != UINT32_MAX) {
          stats.cacheHits++;
          stats.getBitmapTimeUs += micros() - tStart;
          return &slot.buffer[slot.glyphs[mid].bufferOffset];
        }
        break;  // Not extracted during prewarm; fall through to hot-group path
      }
      if (slot.glyphs[mid].glyphIndex < glyphIndex)
        left = mid + 1;
      else
        right = mid - 1;
    }
    break;  // Found the right slot but glyph wasn't in it; don't check other slots
  }

  // Fallback: hot group slot
  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);
  if (groupIndex >= fontData->groupCount) {
    LOG_ERR("FDC", "Glyph %u not found in any group", glyphIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  // Find the group among the hot slots; on a miss inflate it into the least-recently-used one.
  HotGroup* slot = nullptr;
  for (auto& candidate : hotGroups) {
    if (candidate.data != nullptr && candidate.font == fontData && candidate.index == groupIndex) {
      slot = &candidate;
      break;
    }
  }
  if (slot) {
    stats.cacheHits++;
  } else {
    stats.cacheMisses++;
    slot = &hotGroups[0];
    for (auto& candidate : hotGroups) {
      if (candidate.data == nullptr || candidate.font == nullptr) {
        slot = &candidate;
        break;
      }
      if (candidate.lastUse < slot->lastUse) slot = &candidate;
    }
    const EpdFontGroup& group = fontData->groups[groupIndex];

    // ensureCapacity may free the buffer, so the cached-group identity dies with it either way.
    slot->font = nullptr;
    slot->index = UINT16_MAX;
    slot->offsetsValid = false;
    if (!ensureCapacity(slot->data, slot->capacity, group.uncompressedSize)) {
      LOG_ERR("FDC", "Failed to allocate %u bytes for hot group %u", group.uncompressedSize, groupIndex);
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    if (!decompressGroup(fontData, groupIndex, slot->data, group.uncompressedSize)) {
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    slot->font = fontData;
    slot->index = groupIndex;
    stats.hotGroupBytes = group.uncompressedSize;

    // Record where each glyph of this group starts, so the fetch below is a table lookup
    // instead of a walk over every preceding glyph. Entry 0 is 0 by construction, which is
    // also the right fallback when the table cannot be built (see the lookup).
    if (fontData->glyphToGroup) {
      // Frequency-grouped: one pass over the mapping collects the members in glyph order,
      // the order their bitmaps were concatenated in.
      if (ensureCapacity(slot->offsets, slot->offsetsCapacity, group.glyphCount) &&
          ensureCapacity(slot->members, slot->membersCapacity, group.glyphCount)) {
        const uint32_t total = totalGlyphs(fontData);
        uint32_t offset = 0;
        uint32_t count = 0;
        for (uint32_t i = 0; i < total && count < group.glyphCount; ++i) {
          if (fontData->glyphToGroup[i] != groupIndex) continue;
          slot->members[count] = i;
          slot->offsets[count++] = offset;
          const EpdGlyph& g = fontData->glyph[i];
          if (g.width > 0 && g.height > 0) offset += ((g.width + 3) / 4) * g.height;
        }
        slot->offsetsValid = count == group.glyphCount;
      }
    } else if (ensureCapacity(slot->offsets, slot->offsetsCapacity, group.glyphCount)) {
      uint32_t offset = 0;
      for (uint32_t i = 0; i < group.glyphCount; ++i) {
        slot->offsets[i] = offset;
        const EpdGlyph& g = fontData->glyph[group.firstGlyphIndex + i];
        if (g.width > 0 && g.height > 0) offset += ((g.width + (fontData->is4Bit ? 1 : 3)) / (fontData->is4Bit ? 2 : 4)) * g.height;
      }
      slot->offsetsValid = true;
    }
  }
  slot->lastUse = ++hotGroupClock;

  // Compact just the requested glyph from byte-aligned data into scratch buffer
  if (!ensureCapacity(hotGlyphBuf, hotGlyphBufCapacity, glyph->dataLength)) {
    LOG_ERR("FDC", "Failed to allocate %u bytes for glyph scratch", (unsigned)glyph->dataLength);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  uint32_t alignedOff = UINT32_MAX;
  const EpdFontGroup& hotGroupInfo = fontData->groups[groupIndex];
  if (slot->offsetsValid && fontData->glyphToGroup) {
    const uint32_t* first = slot->members;
    const uint32_t* end = first + hotGroupInfo.glyphCount;
    const uint32_t* found = std::lower_bound(first, end, glyphIndex);
    if (found != end && *found == glyphIndex) alignedOff = slot->offsets[found - first];
  } else if (slot->offsetsValid && glyphIndex >= hotGroupInfo.firstGlyphIndex &&
             glyphIndex - hotGroupInfo.firstGlyphIndex < hotGroupInfo.glyphCount) {
    alignedOff = slot->offsets[glyphIndex - hotGroupInfo.firstGlyphIndex];
  }
  if (alignedOff == UINT32_MAX) {
    // A table that could not be built: keep the original walk so behaviour is unchanged.
    alignedOff = getAlignedOffset(fontData, groupIndex, glyphIndex);
  }
  compactSingleGlyph(&slot->data[alignedOff], hotGlyphBuf, glyph->width, glyph->height, fontData->is4Bit);
  stats.getBitmapTimeUs += micros() - tStart;
  return hotGlyphBuf;
}

// --- Prewarm: pre-decompress glyph bitmaps for a page of text ---

int32_t FontDecompressor::findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint) {
  const EpdUnicodeInterval* intervals = fontData->intervals;
  const int count = fontData->intervalCount;

  if (count == 0) return -1;

  // Binary search
  int left = 0;
  int right = count - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const EpdUnicodeInterval* interval = &intervals[mid];

    if (codepoint < interval->first) {
      right = mid - 1;
    } else if (codepoint > interval->last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(interval->offset + (codepoint - interval->first));
    }
  }

  return -1;
}

int FontDecompressor::prewarmCache(const EpdFontData* fontData, const char* utf8Text) {
  if (!fontData || !fontData->groups || !utf8Text) return 0;

  // Allocate the next available slot (caller must call freePageBuffer/clearCache to reset)
  if (pageSlotCount >= MAX_PAGE_SLOTS) {
    LOG_ERR("FDC", "All %u page buffer slots full, cannot prewarm fontData=%p", MAX_PAGE_SLOTS, (void*)fontData);
    return -1;
  }
  PageSlot& slot = pageSlots[pageSlotCount];

  // Step 1: Collect unique glyph indices needed for this page
  uint32_t neededGlyphs[MAX_PAGE_GLYPHS];
  uint16_t glyphCount = 0;
  bool glyphCapWarned = false;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    int32_t glyphIdx = findGlyphIndex(fontData, cp);
    if (glyphIdx < 0) continue;

    // Deduplicate
    bool found = false;
    for (uint16_t i = 0; i < glyphCount; i++) {
      if (neededGlyphs[i] == static_cast<uint32_t>(glyphIdx)) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (glyphCount < MAX_PAGE_GLYPHS) {
        neededGlyphs[glyphCount++] = static_cast<uint32_t>(glyphIdx);
      } else if (!glyphCapWarned) {
        LOG_DBG("FDC", "Glyph cap (%u) reached during prewarm; excess glyphs will use hot-group fallback",
                MAX_PAGE_GLYPHS);
        glyphCapWarned = true;
      }
    }
  }

  // Add ligature output glyphs: if both input codepoints of a ligature pair are
  // in the needed set, the output glyph will be queried during rendering.
  if (fontData->ligaturePairs && fontData->ligaturePairCount > 0) {
    for (uint32_t li = 0; li < fontData->ligaturePairCount && glyphCount < MAX_PAGE_GLYPHS; li++) {
      uint32_t leftCp = fontData->ligaturePairs[li].pair >> 16;
      uint32_t rightCp = fontData->ligaturePairs[li].pair & 0xFFFF;

      int32_t leftIdx = findGlyphIndex(fontData, leftCp);
      int32_t rightIdx = findGlyphIndex(fontData, rightCp);
      if (leftIdx < 0 || rightIdx < 0) continue;

      // Check if both inputs are in neededGlyphs
      bool hasLeft = false, hasRight = false;
      for (uint16_t i = 0; i < glyphCount; i++) {
        if (neededGlyphs[i] == static_cast<uint32_t>(leftIdx)) hasLeft = true;
        if (neededGlyphs[i] == static_cast<uint32_t>(rightIdx)) hasRight = true;
        if (hasLeft && hasRight) break;
      }
      if (!hasLeft || !hasRight) continue;

      int32_t outIdx = findGlyphIndex(fontData, fontData->ligaturePairs[li].ligatureCp);
      if (outIdx < 0) continue;

      // Deduplicate
      bool found = false;
      for (uint16_t i = 0; i < glyphCount; i++) {
        if (neededGlyphs[i] == static_cast<uint32_t>(outIdx)) {
          found = true;
          break;
        }
      }
      if (!found) {
        neededGlyphs[glyphCount++] = static_cast<uint32_t>(outIdx);
      }
    }
  }

  if (glyphCount == 0) return 0;

  // Step 2: Compute total buffer size and collect unique groups
  uint32_t totalBytes = 0;
  uint16_t neededGroups[128];
  uint8_t groupCount = 0;
  bool groupCapWarned = false;

  for (uint16_t i = 0; i < glyphCount; i++) {
    totalBytes += fontData->glyph[neededGlyphs[i]].dataLength;
    uint16_t gi = getGroupIndex(fontData, neededGlyphs[i]);
    bool found = false;
    for (uint8_t j = 0; j < groupCount; j++) {
      if (neededGroups[j] == gi) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (groupCount < 128) {
        neededGroups[groupCount++] = gi;
      } else if (!groupCapWarned) {
        LOG_DBG("FDC", "Group cap (128) reached during prewarm; some groups will use hot-group fallback");
        groupCapWarned = true;
      }
    }
  }

  stats.uniqueGroupsAccessed = groupCount;

  // Step 3: Allocate page buffer and lookup table for this slot
  slot.buffer = static_cast<uint8_t*>(fiFontMalloc(totalBytes));
  slot.glyphs = static_cast<PageGlyphEntry*>(fiFontMalloc(glyphCount * sizeof(PageGlyphEntry)));
  if (!slot.buffer || !slot.glyphs) {
    LOG_ERR("FDC", "Failed to allocate page buffer (%u bytes, %u glyphs)", totalBytes, glyphCount);
    fiFontFree(slot.buffer);
    fiFontFree(slot.glyphs);
    slot = {};
    return glyphCount;
  }
  stats.pageBufferBytes += totalBytes;
  stats.pageGlyphsBytes += glyphCount * sizeof(PageGlyphEntry);

  slot.fontData = fontData;
  slot.glyphCount = glyphCount;
  pageSlotCount++;

  // Initialize lookup entries (bufferOffset = UINT32_MAX means not yet extracted)
  for (uint16_t i = 0; i < glyphCount; i++) {
    slot.glyphs[i] = {neededGlyphs[i], UINT32_MAX, 0};
  }

  // Sort by glyphIndex for binary search in getBitmap()
  for (uint16_t i = 1; i < glyphCount; i++) {
    PageGlyphEntry key = slot.glyphs[i];
    int j = i - 1;
    while (j >= 0 && slot.glyphs[j].glyphIndex > key.glyphIndex) {
      slot.glyphs[j + 1] = slot.glyphs[j];
      j--;
    }
    slot.glyphs[j + 1] = key;
  }

  // Step 3b: Pre-scan to compute each needed glyph's byte-aligned offset within its group.
  // This avoids recomputing aligned offsets per group during extraction in step 4.
  uint32_t groupAlignedTracker[128] = {};  // running byte-aligned offset for each needed group

  if (fontData->glyphToGroup) {
    // Frequency-grouped: single O(totalGlyphs) pass through glyphToGroup
    const uint32_t glyphTotal = totalGlyphs(fontData);
    for (uint32_t i = 0; i < glyphTotal; i++) {
      const uint16_t gi = fontData->glyphToGroup[i];
      // Find this glyph's group position in neededGroups
      uint8_t gpPos = groupCount;
      for (uint8_t j = 0; j < groupCount; j++) {
        if (neededGroups[j] == gi) {
          gpPos = j;
          break;
        }
      }
      if (gpPos == groupCount) continue;  // not a needed group

      const EpdGlyph& glyph = fontData->glyph[i];

      // Binary search in sorted slot.glyphs to find if glyph i is needed
      int left = 0, right = (int)slot.glyphCount - 1;
      while (left <= right) {
        const int mid = left + (right - left) / 2;
        if (slot.glyphs[mid].glyphIndex == i) {
          slot.glyphs[mid].alignedOffset = groupAlignedTracker[gpPos];
          break;
        }
        if (slot.glyphs[mid].glyphIndex < i)
          left = mid + 1;
        else
          right = mid - 1;
      }

      if (glyph.width > 0 && glyph.height > 0) {
        groupAlignedTracker[gpPos] += ((glyph.width + (fontData->is4Bit ? 1 : 3)) / (fontData->is4Bit ? 2 : 4)) * glyph.height;
      }
    }
  } else {
    // Contiguous-group: iterate each needed group's glyphs directly
    for (uint8_t g = 0; g < groupCount; g++) {
      const EpdFontGroup& group = fontData->groups[neededGroups[g]];
      uint32_t alignedOff = 0;
      for (uint16_t j = 0; j < group.glyphCount; j++) {
        const uint32_t glyphI = group.firstGlyphIndex + j;
        const EpdGlyph& glyph = fontData->glyph[glyphI];

        int left = 0, right = (int)slot.glyphCount - 1;
        while (left <= right) {
          const int mid = left + (right - left) / 2;
          if (slot.glyphs[mid].glyphIndex == glyphI) {
            slot.glyphs[mid].alignedOffset = alignedOff;
            break;
          }
          if (slot.glyphs[mid].glyphIndex < glyphI)
            left = mid + 1;
          else
            right = mid - 1;
        }

        if (glyph.width > 0 && glyph.height > 0) {
          alignedOff += ((glyph.width + (fontData->is4Bit ? 1 : 3)) / (fontData->is4Bit ? 2 : 4)) * glyph.height;
        }
      }
    }
  }

  // Step 4: For each unique group, decompress to temp buffer and extract needed glyphs
  uint32_t writeOffset = 0;
  int missed = 0;

  for (uint8_t g = 0; g < groupCount; g++) {
    uint16_t groupIdx = neededGroups[g];
    const EpdFontGroup& group = fontData->groups[groupIdx];

    auto* tempBuf = static_cast<uint8_t*>(fiFontMalloc(group.uncompressedSize));
    if (!tempBuf) {
      LOG_ERR("FDC", "Failed to allocate temp buffer (%u bytes) for group %u", group.uncompressedSize, groupIdx);
      missed++;
      continue;
    }
    if (group.uncompressedSize > stats.peakTempBytes) {
      stats.peakTempBytes = group.uncompressedSize;
    }

    if (!decompressGroup(fontData, groupIdx, tempBuf, group.uncompressedSize)) {
      fiFontFree(tempBuf);
      missed++;
      continue;
    }

    // Extract needed glyphs directly from the byte-aligned temp buffer, compacting on the fly.
    // alignedOffset was pre-computed in step 3b — no full-group compact scan needed.
    for (uint16_t i = 0; i < slot.glyphCount; i++) {
      if (slot.glyphs[i].bufferOffset != UINT32_MAX) continue;  // already extracted
      if (getGroupIndex(fontData, slot.glyphs[i].glyphIndex) != groupIdx) continue;

      const EpdGlyph& glyph = fontData->glyph[slot.glyphs[i].glyphIndex];
      compactSingleGlyph(&tempBuf[slot.glyphs[i].alignedOffset], &slot.buffer[writeOffset], glyph.width, glyph.height, fontData->is4Bit);
      slot.glyphs[i].bufferOffset = writeOffset;
      writeOffset += glyph.dataLength;
    }

    fiFontFree(tempBuf);
  }

  LOG_DBG("FDC", "Prewarm: %u glyphs in %u bytes from %u groups (%d missed)", glyphCount, writeOffset, groupCount,
          missed);

  return missed;
}

// --- Stats ---

void FontDecompressor::resetStats() { stats = Stats{}; }

#if RICKYOS_PAINT_PROFILE
#define FDC_STATS_LOG LOG_INF
#else
#define FDC_STATS_LOG LOG_DBG
#endif
void FontDecompressor::logStats(const char* label) {
  const uint32_t total = stats.cacheHits + stats.cacheMisses;
  FDC_STATS_LOG("FDC", "[%s] hits=%lu misses=%lu (%.1f%% hit rate)", label, stats.cacheHits, stats.cacheMisses,
          total > 0 ? 100.0f * stats.cacheHits / total : 0.0f);
  FDC_STATS_LOG("FDC", "[%s] decompress=%lums groups_accessed=%u", label, stats.decompressTimeMs, stats.uniqueGroupsAccessed);
  FDC_STATS_LOG("FDC", "[%s] mem: pageBuf=%lu pageGlyphs=%lu hotGroup=%lu peakTemp=%lu", label, stats.pageBufferBytes,
          stats.pageGlyphsBytes, stats.hotGroupBytes, stats.peakTempBytes);
  if (stats.getBitmapCalls > 0) {
    FDC_STATS_LOG("FDC", "[%s] getBitmap: %lu calls, %luus total, %luus/call avg", label, stats.getBitmapCalls,
            stats.getBitmapTimeUs, stats.getBitmapTimeUs / stats.getBitmapCalls);
  }
  resetStats();
}
