// Image helpers: PNG output (screenshots, texture dumps for debugging) and the icon's pixels.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace image {
// Writes an 8-bit RGB (channels = 3) or RGBA (channels = 4) PNG, rows top to bottom. false when
// the file could not be written whole (a file it started is removed).
bool writePNG(const std::string& path, int w, int h, int channels, const uint8_t* pixels);

// The uncompressed 32-bit entries of a Windows .ico file (res/icons/scacelith.ico), in the layout
// of the X11 _NET_WM_ICON property: for each entry its width, its height, then width x height
// pixels 0xAARRGGBB (straight alpha), rows top to bottom. Square entries only; PNG entries,
// entries of other depths and entries out of the data's bounds are skipped (nothing for a bad
// header).
std::vector<uint32_t> icoImages(const uint8_t* data, size_t size);
}
