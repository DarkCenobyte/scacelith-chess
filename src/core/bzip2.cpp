#include "bzip2.h"
#include <algorithm>
#include <cstring>

namespace bz2 {

namespace {

constexpr uint64_t kBlockMagic = 0x314159265359ull;   // pi
constexpr uint64_t kEndMagic = 0x177245385090ull;     // sqrt(pi)
constexpr int kMaxGroups = 6;
constexpr int kMaxAlpha = 258;
constexpr int kMaxSelectors = 18002;                  // 2 + 900000 / 50
constexpr int kMaxCodeLen = 20;
constexpr int kGroupSize = 50;
constexpr int kFastBits = 10;

// The CRC of bzip2: CRC-32 with the polynomial 0x04c11db7, most significant bit first.
struct CrcTable {
    uint32_t t[256];
    CrcTable() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04c11db7u : c << 1;
            t[i] = c;
        }
    }
};
const CrcTable kCrc;

inline uint32_t crcByte(uint32_t crc, uint8_t b) { return (crc << 8) ^ kCrc.t[(crc >> 24) ^ b]; }

}  // namespace

Decoder::Decoder(Source source) : source_(std::move(source)), in_(64 * 1024) {}

bool Decoder::fail(const char* why) {
    if (state_ != State::Failed) error_ = why;
    state_ = State::Failed;
    return false;
}

// Tops the bit buffer up to at least 57 bits while input remains. False on a read error.
bool Decoder::refill() {
    while (bitCount_ <= 56) {
        if (inPos_ == inLen_) {
            if (inputEnded_) return true;
            long r = source_(in_.data(), in_.size());
            if (r < 0) return fail("read error");
            if (r == 0) {
                inputEnded_ = true;
                return true;
            }
            inPos_ = 0;
            inLen_ = size_t(r);
            consumed_ += uint64_t(r);
        }
        bitBuf_ = (bitBuf_ << 8) | in_[inPos_++];
        bitCount_ += 8;
    }
    return true;
}

// The next n bits (n <= 32), most significant first. False at the end of the input.
bool Decoder::bits(int n, uint32_t& v) {
    if (bitCount_ < n && (!refill() || bitCount_ < n)) return fail("unexpected end of the compressed data");
    bitCount_ -= n;
    v = uint32_t((bitBuf_ >> bitCount_) & ((uint64_t(1) << n) - 1));
    return true;
}

bool Decoder::readStreamHeader(bool first) {
    // Between streams: the end of the input, or "BZh" again.
    if (!first) {
        bitCount_ -= bitCount_ % 8;   // streams start on a byte boundary
        if (!refill()) return false;
        if (bitCount_ == 0) {
            state_ = State::Done;
            return true;
        }
    }
    uint32_t b, z, h, level;
    if (!bits(8, b) || !bits(8, z) || !bits(8, h) || !bits(8, level)) {
        error_ = first ? "not a bzip2 stream (too short)" : "data after the end of the bzip2 stream";
        return false;
    }
    if (b != 'B' || z != 'Z' || h != 'h' || level < '1' || level > '9')
        return fail(first ? "not a bzip2 stream" : "data after the end of the bzip2 stream");
    blockSize100k_ = int(level - '0');
    combinedCrc_ = 0;
    state_ = State::BlockOrEnd;
    return true;
}

// Canonical Huffman decoding, as in bzip2 (hbCreateDecodeTables), plus a 10-bit lookup table.
bool Decoder::decodeSymbol(const Table& t, int& sym) {
    if (bitCount_ < kMaxCodeLen && !refill()) return false;
    uint32_t peek = bitCount_ >= kFastBits ? uint32_t(bitBuf_ >> (bitCount_ - kFastBits)) & ((1u << kFastBits) - 1)
                                           : uint32_t(bitBuf_ << (kFastBits - bitCount_)) & ((1u << kFastBits) - 1);
    uint16_t e = t.fast[peek];
    if (e) {
        int len = e >> 9;
        if (len > bitCount_) return fail("unexpected end of the compressed data");
        bitCount_ -= len;
        sym = e & 511;
        return true;
    }
    int zn = t.minLen;
    uint32_t zvec;
    if (!bits(zn, zvec)) return false;
    for (;;) {
        if (zn > kMaxCodeLen) return fail("corrupt Huffman code");
        if (int32_t(zvec) <= t.limit[zn]) break;
        uint32_t bit;
        if (!bits(1, bit)) return false;
        ++zn;
        zvec = (zvec << 1) | bit;
    }
    int32_t idx = int32_t(zvec) - t.base[zn];
    if (idx < 0 || idx >= kMaxAlpha) return fail("corrupt Huffman code");
    sym = t.perm[idx];
    return true;
}

// Reads a whole block (its symbols, the inverse MTF and the inverse BWT links) and prepares the
// output walk; or the end of the stream with its CRC.
bool Decoder::readBlock() {
    uint32_t hi, lo;
    if (!bits(24, hi) || !bits(24, lo)) return false;
    uint64_t magic = uint64_t(hi) << 24 | lo;
    if (magic == kEndMagic) {
        uint32_t crc;
        if (!bits(32, crc)) return false;
        if (crc != combinedCrc_) return fail("stream CRC mismatch");
        return readStreamHeader(false);
    }
    if (magic != kBlockMagic) return fail("bad block header");
    uint32_t randomised, origPtr;
    if (!bits(32, blockCrc_) || !bits(1, randomised) || !bits(24, origPtr)) return false;
    if (randomised) return fail("randomised blocks are not supported");

    // The bytes the block uses.
    uint32_t inUse16, w;
    if (!bits(16, inUse16)) return false;
    uint8_t seqToUnseq[256];
    int nInUse = 0;
    for (int i = 0; i < 16; ++i) {
        if (!(inUse16 & (0x8000u >> i))) continue;
        if (!bits(16, w)) return false;
        for (int j = 0; j < 16; ++j)
            if (w & (0x8000u >> j)) seqToUnseq[nInUse++] = uint8_t(i * 16 + j);
    }
    if (nInUse == 0) return fail("block uses no byte");
    const int alphaSize = nInUse + 2;

    // Huffman tables and the table selected for each group of 50 symbols.
    uint32_t nGroups, nSelectors;
    if (!bits(3, nGroups) || !bits(15, nSelectors)) return false;
    if (nGroups < 2 || nGroups > uint32_t(kMaxGroups) || nSelectors < 1) return fail("bad block tables");
    std::vector<uint8_t> selector(std::min<uint32_t>(nSelectors, kMaxSelectors));
    uint8_t pos[kMaxGroups];
    for (int i = 0; i < int(nGroups); ++i) pos[i] = uint8_t(i);
    for (uint32_t i = 0; i < nSelectors; ++i) {
        int j = 0;
        for (;;) {
            uint32_t bit;
            if (!bits(1, bit)) return false;
            if (!bit) break;
            if (++j >= int(nGroups)) return fail("bad selector");
        }
        if (i >= uint32_t(kMaxSelectors)) continue;   // tolerated and ignored, as bzip2 1.0.8 does
        uint8_t v = pos[j];
        for (; j > 0; --j) pos[j] = pos[j - 1];
        pos[0] = v;
        selector[i] = v;
    }
    nSelectors = uint32_t(selector.size());
    Table tables[kMaxGroups];
    for (int t = 0; t < int(nGroups); ++t) {
        uint8_t len[kMaxAlpha];
        uint32_t curr;
        if (!bits(5, curr)) return false;
        for (int i = 0; i < alphaSize; ++i) {
            for (;;) {
                if (curr < 1 || curr > uint32_t(kMaxCodeLen)) return fail("bad code length");
                uint32_t bit;
                if (!bits(1, bit)) return false;
                if (!bit) break;
                if (!bits(1, bit)) return false;
                curr = bit ? curr - 1 : curr + 1;
            }
            len[i] = uint8_t(curr);
        }
        Table& tb = tables[t];
        tb.minLen = 32;
        tb.maxLen = 0;
        for (int i = 0; i < alphaSize; ++i) {
            tb.minLen = std::min<int>(tb.minLen, len[i]);
            tb.maxLen = std::max<int>(tb.maxLen, len[i]);
        }
        int pp = 0;
        for (int l = tb.minLen; l <= tb.maxLen; ++l)
            for (int s = 0; s < alphaSize; ++s)
                if (len[s] == l) tb.perm[pp++] = uint16_t(s);
        for (int s = 0; s < alphaSize; ++s) ++tb.base[len[s] + 1];
        for (int i = 1; i < 24; ++i) tb.base[i] += tb.base[i - 1];
        int32_t vec = 0;
        for (int l = tb.minLen; l <= tb.maxLen; ++l) {
            vec += tb.base[l + 1] - tb.base[l];
            tb.limit[l] = vec - 1;
            vec <<= 1;
        }
        for (int l = tb.minLen + 1; l <= tb.maxLen; ++l) tb.base[l] = ((tb.limit[l - 1] + 1) << 1) - tb.base[l];
        // Short codes straight from a table (the canonical codes, assigned in perm order).
        uint32_t code = 0;
        pp = 0;
        for (int l = tb.minLen; l <= tb.maxLen; ++l) {
            for (int s = 0; s < alphaSize; ++s) {
                if (len[s] != l) continue;
                if (l <= kFastBits && code < (1u << l)) {
                    uint32_t first = code << (kFastBits - l), count = 1u << (kFastBits - l);
                    for (uint32_t k = 0; k < count; ++k) tb.fast[first + k] = uint16_t(l << 9 | s);
                }
                ++code;
            }
            code <<= 1;
        }
    }

    // Symbols: runs of the front byte (RUNA, RUNB in bijective base 2), MTF positions, EOB.
    const int32_t nblockMax = 100000 * blockSize100k_;
    if (int32_t(tt_.size()) < nblockMax) tt_.resize(size_t(nblockMax));
    uint32_t counts[256] = {};
    uint8_t mtf[256];
    for (int i = 0; i < 256; ++i) mtf[i] = uint8_t(i);
    const int eob = nInUse + 1;
    int32_t nblock = 0;
    int groupNo = -1, groupPos = 0;
    const Table* table = nullptr;
    auto nextSym = [&](int& sym) {
        if (groupPos == 0) {
            if (++groupNo >= int(nSelectors)) return fail("too few selectors");
            groupPos = kGroupSize;
            table = &tables[selector[size_t(groupNo)]];
        }
        --groupPos;
        return decodeSymbol(*table, sym);
    };
    int sym;
    if (!nextSym(sym)) return false;
    for (;;) {
        if (sym == eob) break;
        if (sym <= 1) {   // RUNA = 0, RUNB = 1
            int32_t es = -1, n = 1;
            do {
                if (n >= 2 * 1024 * 1024) return fail("run too long");
                es += (sym + 1) * n;
                n <<= 1;
                if (!nextSym(sym)) return false;
            } while (sym <= 1);
            ++es;
            uint8_t uc = seqToUnseq[mtf[0]];
            if (es > nblockMax - nblock) return fail("block too long");
            counts[uc] += uint32_t(es);
            for (int32_t k = 0; k < es; ++k) tt_[size_t(nblock++)] = uc;
            continue;
        }
        if (nblock >= nblockMax) return fail("block too long");
        int nn = sym - 1;
        if (nn >= nInUse) return fail("bad MTF symbol");
        uint8_t v = mtf[nn];
        std::memmove(mtf + 1, mtf, size_t(nn));
        mtf[0] = v;
        uint8_t uc = seqToUnseq[v];
        ++counts[uc];
        tt_[size_t(nblock++)] = uc;
        if (!nextSym(sym)) return false;
    }
    if (origPtr >= uint32_t(nblock)) return fail("bad block origin");

    // Inverse BWT: each entry keeps its byte in the low 8 bits and the next position above.
    uint32_t cftab[256];
    uint32_t sum = 0;
    for (int i = 0; i < 256; ++i) {
        cftab[i] = sum;
        sum += counts[i];
    }
    for (int32_t i = 0; i < nblock; ++i) {
        uint8_t uc = uint8_t(tt_[size_t(i)] & 0xff);
        tt_[cftab[uc]++] |= uint32_t(i) << 8;
    }
    tPos_ = tt_[origPtr] >> 8;
    nblock_ = nblock;
    used_ = 0;
    runLength_ = 0;
    repeat_ = 0;
    crc_ = 0xffffffffu;
    state_ = State::Output;
    return true;
}

long Decoder::read(uint8_t* out, size_t n) {
    size_t produced = 0;
    while (produced < n) {
        switch (state_) {
            case State::Failed: return -1;
            case State::Done: return long(produced);
            // Every failure below has gone through fail() (state Failed, error_ set).
            case State::StreamHeader:
                if (!readStreamHeader(true)) return -1;
                break;
            case State::BlockOrEnd:
                if (!readBlock()) return -1;
                break;
            case State::Output: {
                // The BWT walk, undoing the initial run-length coding (four equal bytes, then the
                // count of further copies), CRC as the bytes go out.
                uint32_t crc = crc_, t = tPos_;
                int32_t used = used_;
                int run = runLength_, rep = repeat_;
                uint8_t last = last_;
                const uint32_t* tt = tt_.data();
                while (produced < n) {
                    if (rep > 0) {
                        out[produced++] = last;
                        crc = crcByte(crc, last);
                        --rep;
                        continue;
                    }
                    if (used == nblock_) break;
                    t = tt[t];
                    uint8_t b = uint8_t(t & 0xff);
                    t >>= 8;
                    ++used;
                    if (run == 4) {
                        rep = b;
                        run = 0;
                        continue;
                    }
                    out[produced++] = b;
                    crc = crcByte(crc, b);
                    if (run > 0 && b == last) {
                        ++run;
                    } else {
                        last = b;
                        run = 1;
                    }
                }
                crc_ = crc;
                tPos_ = t;
                used_ = used;
                runLength_ = run;
                repeat_ = rep;
                last_ = last;
                if (used == nblock_ && rep == 0) {
                    if (~crc_ != blockCrc_) {
                        fail("block CRC mismatch (corrupt data)");
                        return -1;
                    }
                    combinedCrc_ = ((combinedCrc_ << 1) | (combinedCrc_ >> 31)) ^ blockCrc_;
                    state_ = State::BlockOrEnd;
                }
                break;
            }
        }
    }
    return long(produced);
}

bool decompress(const std::string& in, std::string& out, std::string* error) {
    size_t at = 0;
    Decoder d([&](uint8_t* buf, size_t n) {
        size_t k = std::min(n, in.size() - at);
        std::memcpy(buf, in.data() + at, k);
        at += k;
        return long(k);
    });
    out.clear();
    uint8_t buf[65536];
    for (;;) {
        long r = d.read(buf, sizeof buf);
        if (r < 0) {
            if (error) *error = d.error();
            return false;
        }
        if (r == 0) return true;
        out.append(reinterpret_cast<char*>(buf), size_t(r));
    }
}

}  // namespace bz2
