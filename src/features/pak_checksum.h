#ifndef COD1RELOADED_PAK_CHECKSUM_H
#define COD1RELOADED_PAK_CHECKSUM_H

// pak_checksum - the number a pure server lists for a pk3 in sv_paks / sv_referencedPaks.
//
// Not the file's bytes: FS_LoadZipFile takes the CRC-32 of every file with a non-zero size,
// in central-directory order, and folds them with Com_BlockChecksum (MD4, the four digest
// words xored). Same algorithm as cod1plushookserver/scripts/gen_pure_extra.py.
//
// Pure computation, reads the zip's central directory only. Safe on any thread.

#include <cstdint>

namespace patches {

// false when the file is not a readable zip
bool pak_checksum(const char* path, int32_t* out);

}  // namespace patches

#endif
