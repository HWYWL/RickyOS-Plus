#pragma once

class GfxRenderer;

// Work that otherwise lands on the first visits after a new firmware or a card changed
// elsewhere: the library index walk (Library and Storage would each start it, with
// only a static popup, for as long as a large or unsorted card takes) and the recent
// books' cover thumbnails (a firmware that changes their format regenerates every one
// on the first Home visit). Run on the boot screen with a progress line, so the first
// minutes of use are not the slow ones.
namespace BootPreparation {

// Does nothing on an ordinary boot: only when this firmware has not prepared the card
// yet, or the library index is missing, stale or built in the other metadata mode.
void run(GfxRenderer& renderer);

}  // namespace BootPreparation
