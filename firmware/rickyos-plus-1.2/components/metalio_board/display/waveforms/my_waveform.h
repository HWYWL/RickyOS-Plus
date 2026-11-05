#pragma once

#include "epd_internals.h"

/** GC16/GL16 + 完备 DU；GL16_FAST 为阅读专用 37 相裁剪/保持对角线实验表。 */
extern const EpdWaveform MY_WAVEFORM;

// Regional UI updates: text-turn table with an isolated black saturation margin.
#define MY_MODE_UI_TURN 0xB  // the MODE_GL16_INV slot in epdiy.h
// Reader page turns: the original text-turn table plus a short re-drive of retained black.
#define MY_MODE_READER_TURN 0x3  // the MODE_GC16_FAST slot in epdiy.h
// Port-private waveform slot, not a packing flag; background gray retains its erase.
#define MY_MODE_READER_PAPER_TURN 0xC

/**
 * @brief 开机装配 MY_WAVEFORM（DU 完备；GC/GL 按 MY_WAVEFORM_TRIM 裁或拷全表）
 * @note 须在 epd_hl_init(&MY_WAVEFORM) 之前调用一次
 */
void my_waveform_init(void);
