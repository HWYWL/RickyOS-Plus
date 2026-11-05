#pragma once
#include "Rect.h"
#include "UiAppHost.h"

namespace RickyProfile {
using UiScreen = UiAppHost::UiScreen;
const char* nickname();
const char* homePhrase();
bool setHomePhrase(const std::string& text);
void drawAvatar(const GfxRenderer& renderer, const Rect& rect);
// Built-in animal portraits live in the avatar setting as "@avatar:<index>"; an empty
// setting is the default, the puppy (index DEFAULT_AVATAR). Returns the portrait shown,
// or -1 for an imported image.
inline constexpr int DEFAULT_AVATAR = 0;
int presetAvatarIndex();
void drawPresetAvatar(const GfxRenderer& renderer, const Rect& rect, int index);
// "" restores the default puppy. Saves settings; the previous value returns on failure.
bool setAvatar(const char* value);
// The card's visible rectangle is also its only touch target.
void drawCard(UiScreen& screen, const GfxRenderer& renderer, freeink::ui::Rect rect, freeink::ui::ActionId action,
              int value = 0);
bool importAvatar(const std::string& path);
}  // namespace RickyProfile
