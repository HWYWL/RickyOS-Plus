#pragma once

namespace UserGuide {
// Both hooks are no-ops in RickyOS products; existing card contents are preserved.
// Called once after loading recents. An unreadable existing store is never an empty library.
void prepare(bool recentsLoaded);
// Called before routing home, after onboarding. At most one installation attempt per boot.
void installIfPending(bool simplifiedChinese);
}  // namespace UserGuide
