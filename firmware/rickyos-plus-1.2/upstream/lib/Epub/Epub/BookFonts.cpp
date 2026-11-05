#include "BookFonts.h"

namespace bookfonts {
namespace {
Resolver* activeResolver = nullptr;
}

void setActive(Resolver* resolver) { activeResolver = resolver; }
Resolver* active() { return activeResolver; }

}  // namespace bookfonts
