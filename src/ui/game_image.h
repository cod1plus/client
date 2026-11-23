#ifndef COD1RELOADED_GAME_IMAGE_H
#define COD1RELOADED_GAME_IMAGE_H
// Decoders for the game's own image formats, so the overlay can draw the stock HUD art
// (weapon silhouettes, objective icons) read out of the paks by the engine's filesystem:
//   DDS  DXT1 / DXT3 / DXT5 and uncompressed 24/32-bit
//   TGA  truecolour and greyscale, raw or RLE
// Pure code (no engine, no GL): the offline preview uses it as is.
#include <vector>

namespace patches {

// Decodes `data` into rows top-down, bytes R,G,B,A. `path` only picks the format by its
// extension (".dds" / ".tga"); anything else is refused (GDI+ handles JPG/PNG).
bool game_image_decode(const char* path, const unsigned char* data, int len,
                       std::vector<unsigned char>& rgba, int& w, int& h);

}  // namespace patches

#endif
