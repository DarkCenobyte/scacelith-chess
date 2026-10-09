#include "image.h"
#include "files.h"
#include <algorithm>
#include <cstring>
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
    // A screenshot, written where the player asked (--shot) or in their own settings folder.
    FILE* f = files::create(path.c_str());
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

namespace {
uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t le32(const uint8_t* p) { return le16(p) | le16(p + 2) << 16; }
}  // namespace

// ICONDIR (reserved 0, type 1, count), ICONDIRENTRY x count (16 bytes: ..., size at 8, offset at
// 12), then each image: a PNG, or a BITMAPINFOHEADER with the height doubled (the AND mask
// follows the pixels), bottom-up BGRA rows for 32 bits per pixel.
std::vector<uint32_t> icoImages(const uint8_t* d, size_t n) {
    std::vector<uint32_t> out;
    if (!d || n < 6 || le16(d) != 0 || le16(d + 2) != 1) return out;
    const size_t count = le16(d + 4);
    for (size_t i = 0; i < count && 6 + 16 * (i + 1) <= n; ++i) {
        const uint8_t* e = d + 6 + 16 * i;
        const size_t len = le32(e + 8), off = le32(e + 12);
        if (off > n || len > n - off || len < 40) continue;
        const uint8_t* b = d + off;
        if (std::memcmp(b, "\x89PNG", 4) == 0) continue;
        const size_t header = le32(b);
        const int32_t w = int32_t(le32(b + 4)), h2 = int32_t(le32(b + 8));
        if (header < 40 || header > len || le16(b + 14) != 32 || le32(b + 16) != 0 || w <= 0 || w > 256 || h2 != 2 * w)
            continue;
        const size_t row = size_t(w) * 4;
        if (size_t(w) * row > len - header) continue;
        out.push_back(uint32_t(w));
        out.push_back(uint32_t(w));
        for (int32_t y = w - 1; y >= 0; --y) {
            const uint8_t* p = b + header + size_t(y) * row;
            for (int32_t x = 0; x < w; ++x, p += 4)
                out.push_back(uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | uint32_t(p[0]));
        }
    }
    return out;
}
}  // namespace image
