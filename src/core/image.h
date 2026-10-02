// Image output helpers (screenshots, texture dumps for debugging).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace image {
// Writes an 8-bit RGB (channels = 3) or RGBA (channels = 4) PNG, rows top to bottom. false when
// the file could not be written whole (a file it started is removed).
bool writePNG(const std::string& path, int w, int h, int channels, const uint8_t* pixels);
}
