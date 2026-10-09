// stb_truetype's implementation, for the font atlas (src/ui/ui_font.cpp includes only the
// declarations). In a unit of its own, the vendored header is compiled unmodified and stays out of
// the CodeQL scan like the rest of third_party/ (README.scacelith.md).
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
