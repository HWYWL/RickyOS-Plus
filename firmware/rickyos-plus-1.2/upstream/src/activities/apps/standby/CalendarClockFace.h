#pragma once

#include <cstdint>

#include "StandbyFace.h"

// RickyOS desk clock for a stand: landscape, the time large on the left with the
// battery and date under it, this month's calendar on the right with today marked.
// Pure B/W, so the minute updates use the quiet B/W waveform instead of redrawing a
// 16-gray frame (CrossPointSettings::rickyStandbyFace).
class CalendarClockFace final : public StandbyFace {
 public:
  void onEnter() override { lastMinute_ = -1; }
  TickResult tick() override;
  void render(GfxRenderer& renderer, const Rect& viewport) override;
  StrId titleId() const override { return StrId::STR_STANDBY_TITLE; }
  uint32_t secondsUntilNextWake() const override;

 private:
  void drawClock(GfxRenderer& renderer, const Rect& area) const;
  void drawCalendar(GfxRenderer& renderer, const Rect& area) const;

  int32_t lastMinute_ = -1;  // local minute of day last drawn
};

