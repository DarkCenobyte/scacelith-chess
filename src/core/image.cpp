#include "image.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace image {
namespace {
uint32_t crcTable[256];
bool crcInit = false;
uint32_t crc(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
    if (!crcInit) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crcTable[i] = v;
        }
        crcInit = true;
    }
    for (size_t i = 0; i < n; ++i) c = crcTable[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}
void be32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(uint8_t(v >> 24)); o.push_back(uint8_t(v >> 16)); o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v));
}
// Length, type, data, CRC of type and data; the data is written where it is, not copied. False
// when a write failed.
bool chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> head, tail;
    be32(head, uint32_t(data.size()));
    head.insert(head.end(), type, type + 4);
    be32(tail, crc(data.data(), data.size(), crc(head.data() + 4, 4)) ^ 0xFFFFFFFFu);
    bool ok = std::fwrite(head.data(), 1, head.size(), f) == head.size();
    if (ok && !data.empty()) ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return ok && std::fwrite(tail.data(), 1, tail.size(), f) == tail.size();
}
}  // namespace

// The path is UTF-8: opened as a wide path on Windows (u8path), whatever the process code page.
bool writePNG(const std::string& path, int w, int h, int ch, const uint8_t* px) {
    const std::filesystem::path file = std::filesystem::u8path(path);
#ifdef _WIN32
    FILE* f = _wfopen(file.c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return false;
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    bool ok = std::fwrite(sig, 1, 8, f) == 8;
    std::vector<uint8_t> ihdr;
    be32(ihdr, uint32_t(w));
    be32(ihdr, uint32_t(h));
    ihdr.push_back(8);
    ihdr.push_back(ch == 4 ? 6 : 2);
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    ok = ok && chunk(f, "IHDR", ihdr);
    // zlib stream with stored (uncompressed) deflate blocks.
    std::vector<uint8_t> raw;
    size_t stride = size_t(w) * size_t(ch);
    raw.reserve((stride + 1) * size_t(h));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), px + size_t(y) * stride, px + size_t(y + 1) * stride);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t blocks = std::max<size_t>(1, (raw.size() + 65534) / 65535);
    z.reserve(2 + 5 * blocks + raw.size() + 4);
    size_t pos = 0;
    while (pos < raw.size() || raw.empty()) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
        pos += n;
        if (raw.empty()) break;
    }
    // Adler-32, reduced every 5552 bytes (zlib's NMAX: the sums cannot overflow 32 bits sooner).
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw.size();) {
        size_t end = std::min<size_t>(raw.size(), i + 5552);
        for (; i < end; ++i) { a += raw[i]; b += a; }
        a %= 65521; b %= 65521;
    }
    be32(z, (b << 16) | a);
    ok = ok && chunk(f, "IDAT", z);
    ok = ok && chunk(f, "IEND", {});
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {   // a full disk: no truncated file left behind
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }
    return ok;
}
}  // namespace image
