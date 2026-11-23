#ifndef COD1RELOADED_HUNK_MIN_H
#define COD1RELOADED_HUNK_MIN_H

namespace patches {

// The client's com_hunkMegs minimum goes from 80 to 256 MB: the default 128 dies loading
// mp_liberation (166.6 MB of hunk). A higher value in the config is kept. Before Com_Init (DllMain).
bool hunk_min_install();

}  // namespace patches

#endif
