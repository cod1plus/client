#ifndef COD1RELOADED_DEMO_LIBRARY_H
#define COD1RELOADED_DEMO_LIBRARY_H

// demo_library - the demos on disk and what is inside them, for the Demos tab.
//
// The file list is cheap and rebuilt on demand (main thread). What is inside a demo
// (map, recorder, duration, kills) comes from features/demo_index on a worker thread,
// one file at a time, newest first, and only while the tab is on screen (or for the
// demo the player clicked). Results are kept in cod1reloaded-demos.idx next to the exe,
// keyed by path + size + write time, so a demo is read once, ever.

#include "features/demo_index.h"

#include <cstdint>
#include <string>
#include <vector>

namespace patches {

enum DemoState { DEMO_UNKNOWN = 0, DEMO_INDEXING = 1, DEMO_READY = 2, DEMO_FAILED = 3 };

struct DemoItem {
    std::string name;            // demo0105.dm_2
    std::string dir;             // search-path folder: Main, or a mod folder
    std::string path;            // full path
    uint64_t    size = 0;
    uint64_t    mtime = 0;       // FILETIME, 100 ns since 1601
    int         state = DEMO_UNKNOWN;
    DemoInfo    info;            // valid when state == DEMO_READY (or FAILED: error)
};

// Rebuild the file list (keeps what is already known about unchanged files).
void demo_library_scan();
// The list, newest first. Main thread only; stable until the next scan.
std::vector<DemoItem>& demo_library_items();
// Call every frame the tab is drawn: pulls finished results in, keeps the worker going.
void demo_library_pump(bool tab_visible);
// Index this one next, even if the tab is closed.
void demo_library_request(int index);
// Files known / files indexed (for a "scanning 12/108" line).
void demo_library_progress(int* ready, int* total);

}  // namespace patches

#endif
