#pragma once
#include <I18n.h>

#include "CrossPointSettings.h"

// Profile -> Theme: the dog or the cheese cat. Each dog line has a cat twin; pick one here so
// callers never test the setting themselves.
namespace RickyCompanion {
inline bool cat() { return SETTINGS.rickyCat(); }
inline StrId pick(const StrId dog, const StrId catId) { return cat() ? catId : dog; }
inline const char* text(const StrId dog, const StrId catId) { return I18n::getInstance().get(pick(dog, catId)); }
}  // namespace RickyCompanion
