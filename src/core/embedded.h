// Files embedded into the executable at build time (shaders/, assets/). The build generates a
// table from every file under shaders/ and assets/ (see cmake/embed.cmake). Paths are relative
// to the repository root with forward slashes, e.g. "shaders/include/common.glsl".
#pragma once
#include <cstddef>
#include <string>

namespace embedded {
struct File { const char* path; const unsigned char* data; size_t size; };
// Returns nullptr when not found. Data is always followed by a terminating 0 byte.
const File* find(const char* path);
// Convenience: returns the file as a string, or empty string (and logs an error) if missing.
std::string text(const char* path);
// All embedded files (for listing / hot reload).
const File* all(size_t* count);

// Development hot-reload: when set (from --data-dir <repo root>), text() reads files from disk
// first so shaders can be edited without rebuilding.
void setOverrideDir(const std::string& dir);
}  // namespace embedded
