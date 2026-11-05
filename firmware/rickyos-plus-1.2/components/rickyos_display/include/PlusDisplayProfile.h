#pragma once
#include <cstdint>
namespace freeink {
// 1 = white-page turn, 2 = clean, 4 = regional UI, 5 = reading-background turn.
void metalioPlusSetGray16Profile(uint8_t profile);
void metalioPlusSetPageTurn(uint8_t style, uint8_t orientation, bool fromRight, int originX = -1, int originY = -1);
void metalioPlusSetMainTabTurn(uint8_t orientation, bool fromRight, int x, int y, int width, int height);
void metalioPlusSetGray16UiRefresh(int x, int y, int width, int height, uint8_t contentEvery, uint8_t smallEvery,
                                  bool adaptive = false);
void metalioPlusSetGray16ReaderRefresh(int x, int y, int width, int height);
void metalioPlusSetGray16ReaderTextFinish(bool enabled);
// 快刷: white-page reading turns use the UI's short text-turn table (unchanged pixels
// held, old and new cross-fade) with no motion, instead of the full reader waveform.
// Persistent until changed.
void metalioPlusSetReaderFastRefresh(bool on);
bool metalioPlusRestoreGray16Baseline(uint8_t* proxy);
}
