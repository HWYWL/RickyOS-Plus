#pragma once

#include <cstdint>
#include <string>

#include "ReadingCard.h"
#include "StandbyFace.h"

// RickyOS Standby: the user's own picture (chosen on the Apps → Standby page, stored
// as /sleep.bmp) or the cover of the book being read, in native 16-gray, or the
// RickyOS rest screen until there is one, with an optional date or time-and-date
// corner (CrossPointSettings::standbyOverlay). Fit or fill and the filter are the
// power-off screen's (sleepScreenCoverMode, sleepScreenCoverFilter), which shows the
// same picture.
//
// Each update (the first draw, then every minute for the time or every day for the
// date) redraws the whole picture through the 16-gray frame with the corner on top:
// this panel has no partial refresh, and a B/W refresh would flatten the grays.
class WallpaperFace final : public StandbyFace {
 public:
  void onEnter() override;
  void onExit() override { card_.release(); }
  TickResult tick() override;
  void render(GfxRenderer& renderer, const Rect& viewport) override;
  bool renderNative(GfxRenderer& renderer, const Rect& viewport) override;
  StrId titleId() const override { return StrId::STR_STANDBY_TITLE; }
  uint32_t secondsUntilNextWake() const override;
  bool wantsClock() const override;

 private:
  void drawCorner(GfxRenderer& renderer, const Rect& viewport, bool intoGray) const;

  std::string path_;  // the picture or the cover BMP
  ReadingCard card_;  // the cover face's "上次读到这里" layouts
  bool cardOn_ = false;
  bool hasPicture_ = false;
  int32_t lastMinute_ = -1;  // local minute of day last drawn, -1 before the first draw
  int32_t lastDay_ = -1;     // local day of year last drawn
  bool grayShown_ = false;   // a full 16-gray frame is on the glass; minute updates can be regional
};

