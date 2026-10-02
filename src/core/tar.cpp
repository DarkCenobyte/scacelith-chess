#include "tar.h"
#include <algorithm>
#include <cstring>

namespace tar {

namespace {

constexpr size_t kBlock = 512;
constexpr uint64_t kMaxText = 1 << 20;   // long names and pax headers: far below this
constexpr uint64_t kMaxPaxSize = uint64_t(1) << 62;   // so that left_ + padding_ cannot wrap

// An octal number field (spaces or NULs around it), or GNU base-256 when its top bit is set.
bool number(const uint8_t* f, size_t len, uint64_t& out) {
    out = 0;
    if (f[0] & 0x80) {
        if (f[0] == 0xff) return false;   // negative
        out = f[0] & 0x7f;
        for (size_t i = 1; i < len; ++i) {
            if (out >> 55) return false;
            out = out << 8 | f[i];
        }
        return true;
    }
    size_t i = 0;
    while (i < len && (f[i] == ' ' || f[i] == 0)) ++i;
    for (; i < len && f[i] >= '0' && f[i] <= '7'; ++i) {
        if (out >> 60) return false;
        out = out * 8 + uint64_t(f[i] - '0');
    }
    for (; i < len; ++i)
        if (f[i] != ' ' && f[i] != 0) return false;
    return true;   // an empty field is 0
}

std::string field(const uint8_t* f, size_t len) {
    size_t n = 0;
    while (n < len && f[n]) ++n;
    return std::string(reinterpret_cast<const char*>(f), n);
}

// A pax decimal in s[from, to): digits only (strtoull would also take spaces and a sign), at
// most max. False when empty, malformed or too large.
bool decimal(const std::string& s, size_t from, size_t to, uint64_t max, uint64_t& out) {
    out = 0;
    if (from >= to) return false;
    for (size_t i = from; i < to; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        uint64_t d = uint64_t(s[i] - '0');
        if (out > (max - d) / 10) return false;
        out = out * 10 + d;
    }
    return true;
}

}  // namespace

bool safePath(const std::string& path) {
    if (path.empty() || path[0] == '/' || (path.size() >= 2 && path[1] == ':')) return false;
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        std::string part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char c : part)
            if (c < 0x20 || c == '\\' || c == 0x7f) return false;
        start = end + 1;   // a trailing '/' ends the loop here
    }
    return true;
}

Reader::Reader(Source source) : source_(std::move(source)) {}

bool Reader::readFully(uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        long r = source_(buf + got, n - got);
        if (r < 0) {
            if (error_.empty()) error_ = "read error";
            return false;
        }
        if (r == 0) {
            if (error_.empty()) error_ = "truncated archive";
            return false;
        }
        got += size_t(r);
    }
    return true;
}

bool Reader::skip(uint64_t n) {
    uint8_t buf[16 * 1024];
    while (n) {
        size_t k = size_t(std::min<uint64_t>(n, sizeof buf));
        if (!readFully(buf, k)) return false;
        n -= k;
    }
    return true;
}

bool Reader::readText(uint64_t size, std::string& out) {
    if (size > kMaxText) {
        error_ = "oversized long name or pax header";
        return false;
    }
    out.resize(size_t(size));
    if (size && !readFully(reinterpret_cast<uint8_t*>(&out[0]), size_t(size))) return false;
    if (!skip((kBlock - size % kBlock) % kBlock)) return false;
    return true;
}

bool Reader::next(Entry& e) {
    if (ended_ || !error_.empty()) return false;
    if (!skip(left_ + padding_)) return false;
    left_ = padding_ = 0;
    std::string longName, longLink, paxPath, paxLink;
    uint64_t paxSize = 0;
    bool hasPaxSize = false;
    for (;;) {
        uint8_t h[kBlock];
        // The end: zero blocks, or (leniently) no more data at a block boundary.
        long first = source_(h, kBlock);
        if (first == 0) {
            ended_ = true;
            return false;
        }
        if (first < 0 || (size_t(first) < kBlock && !readFully(h + first, kBlock - size_t(first)))) {
            if (error_.empty()) error_ = "read error";
            return false;
        }
        bool zero = true;
        for (uint8_t b : h) zero = zero && b == 0;
        if (zero) {
            ended_ = true;
            return false;
        }
        uint64_t stored = 0;
        if (!number(h + 148, 8, stored)) {
            error_ = "bad header checksum field";
            return false;
        }
        uint64_t sumU = 0;
        int64_t sumS = 0;
        for (size_t i = 0; i < kBlock; ++i) {
            uint8_t b = (i >= 148 && i < 156) ? uint8_t(' ') : h[i];
            sumU += b;
            sumS += int8_t(b);
        }
        if (stored != sumU && int64_t(stored) != sumS) {
            error_ = "bad header checksum";
            return false;
        }
        uint64_t size = 0;
        if (!number(h + 124, 12, size)) {
            error_ = "bad entry size";
            return false;
        }
        char type = char(h[156]);
        if (type == 'L' || type == 'K') {   // GNU: the name (or link target) of the next entry
            std::string text;
            if (!readText(size, text)) return false;
            text = text.substr(0, text.find('\0'));
            (type == 'L' ? longName : longLink) = text;
            continue;
        }
        if (type == 'x' || type == 'g') {   // pax: "<length> <key>=<value>\n" records
            std::string text;
            if (!readText(size, text)) return false;
            if (type == 'g') continue;   // global defaults: nothing used from them
            size_t pos = 0;
            while (pos < text.size()) {
                size_t sp = text.find(' ', pos);
                if (sp == std::string::npos) break;
                uint64_t len = 0;
                if (!decimal(text, pos, sp, kMaxText, len) || len < sp - pos + 2 || len > text.size() - pos ||
                    text[pos + len - 1] != '\n') {
                    error_ = "bad pax header";
                    return false;
                }
                std::string rec = text.substr(sp + 1, pos + len - 1 - (sp + 1));
                size_t eq = rec.find('=');
                if (eq != std::string::npos) {
                    std::string key = rec.substr(0, eq), value = rec.substr(eq + 1);
                    if (key == "path") paxPath = value;
                    else if (key == "linkpath") paxLink = value;
                    else if (key == "size") {
                        if (!decimal(value, 0, value.size(), kMaxPaxSize, paxSize)) {
                            error_ = "bad pax header";
                            return false;
                        }
                        hasPaxSize = true;
                    }
                }
                pos += size_t(len);
            }
            continue;
        }
        std::string name = field(h, 100);
        const bool posix = std::memcmp(h + 257, "ustar\0", 6) == 0;
        if (posix && h[345]) name = field(h + 345, 155) + "/" + name;
        e = Entry();
        e.path = !paxPath.empty() ? paxPath : !longName.empty() ? longName : name;
        e.type = type;
        e.size = hasPaxSize ? paxSize : size;
        e.linkTarget = !paxLink.empty() ? paxLink : !longLink.empty() ? longLink : field(h + 157, 100);
        // Links, devices, directories and FIFOs carry no data whatever their size field says.
        bool data = !(type >= '1' && type <= '6');
        left_ = data ? e.size : 0;
        padding_ = data ? (kBlock - e.size % kBlock) % kBlock : 0;
        if (!data) e.size = 0;
        return true;
    }
}

long Reader::read(uint8_t* buf, size_t n) {
    if (!error_.empty()) return -1;
    if (left_ == 0 || n == 0) return 0;
    size_t k = size_t(std::min<uint64_t>(n, left_));
    long r = source_(buf, k);
    if (r <= 0) {
        error_ = r < 0 ? "read error" : "truncated archive";
        return -1;
    }
    left_ -= uint64_t(r);
    return r;
}

}  // namespace tar
