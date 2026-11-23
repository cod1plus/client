#ifndef COD1RELOADED_WORKSHOP_TAB_H
#define COD1RELOADED_WORKSHOP_TAB_H
// Ctrl+M > Workshop: the catalog of features/workshop.h as a grid of tiles (picture,
// name, author, size, state) and the selected item's page (big picture, description,
// DOWNLOAD / UPDATE with its progress). Uses only the overlay draw API, so
// tools/workshop_preview renders it offline to a PNG.

namespace patches {

// The tab body inside the menu's content frame: [x, x + w] wide, from y down to bottom.
void workshop_tab_draw(float x, float y, float w, float bottom);

}  // namespace patches

#endif
