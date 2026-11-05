#include "KeyboardLayoutSet.h"

namespace keyboard_layouts {

// RickyOS ships Chinese and English only: every keyboard is English QWERTY, and the
// language key switches the Chinese input method, not the layout. The layout picker
// (Settings -> Keyboard layouts) is gone; its other scripts drew as missing glyphs.
uint16_t enabled() { return bitAt(0); }

freeink::ui::KeyboardLayoutId startingLayout() { return freeink::ui::KeyboardLayoutId::QwertyEn; }

freeink::ui::KeyboardLayoutId next(freeink::ui::KeyboardLayoutId) { return freeink::ui::KeyboardLayoutId::QwertyEn; }

}  // namespace keyboard_layouts
