#pragma once

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "components/RickyBadgeStatus.h"
#include "components/UITheme.h"

// Announces each RickyOS badge once, when a reading session ends.
namespace rickyBadgeNotice {

// First boot with badges: everything already earned counts as announced, so a long
// history does not open with a run of popups.
inline void seedOnce() {
  if (SETTINGS.rickyBadgesSeen & rickyBadgeStatus::kSeeded) return;
  SETTINGS.rickyBadgesSeen = rickyBadges::unlockedMask(rickyBadgeStatus::evaluate()) | rickyBadgeStatus::kSeeded;
  SETTINGS.saveToFile();
}

// Shows up to two newly earned badges (any others are still marked announced; the
// badges page lists them all). Returns whether anything was shown.
inline bool announceNew(const GfxRenderer& renderer) {
  seedOnce();
  const uint32_t earned = rickyBadges::unlockedMask(rickyBadgeStatus::evaluate());
  const uint32_t fresh = earned & ~SETTINGS.rickyBadgesSeen;
  if (fresh == 0) return false;
  int shown = 0;
  char text[96];
  for (size_t i = 0; i < rickyBadges::kCount && shown < 2; ++i) {
    if ((fresh & (1U << i)) == 0) continue;
    snprintf(text, sizeof(text), tr(STR_RICKY_BADGES_UNLOCKED), rickyBadgeStatus::nameOf(i));
    GUI.drawPopup(renderer, text);
    delay(900);
    ++shown;
  }
  SETTINGS.rickyBadgesSeen |= fresh;
  SETTINGS.saveToFile();
  return true;
}

}  // namespace rickyBadgeNotice
