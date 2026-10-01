// bzip2 decompression, streaming (the coach's voice model comes as a .tar.bz2 release archive
// when Hugging Face cannot be reached, src/tts/model_store.h). Written for the project from the
// format of bzip2 1.0 (Julian Seward): stream header "BZh1".."BZh9", blocks of Huffman-coded MTF
// and run-length symbols over a Burrows-Wheeler transform, the initial run-length coding, the
// CRC of every block and of the stream. Concatenated streams (pbzip2, lbzip2) are read one after
// the other; randomised blocks (written only by bzip2 0.9.0 and older) are refused.
//
// Memory: one block at a time (4 bytes per byte of block, 3.6 MB for "BZh9"), whatever the
// size of the archive.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bz2 {

class Decoder {
public:
    // Pulls compressed bytes: returns the count read into buf (at most n), 0 at the end of the
    // input, -1 on a read error.
    using Source = std::function<long(uint8_t* buf, size_t n)>;
    explicit Decoder(Source source);

    // Decompressed bytes into out (at most n): the count, 0 once the last stream ended cleanly
    // (end of input after it), -1 on an error (error() says which; a corrupt stream always ends
    // in an error, at the latest at the CRC check of its block).
    long read(uint8_t* out, size_t n);
    const std::string& error() const { return error_; }
    uint64_t consumed() const { return consumed_; }   // compressed bytes pulled from the source

private:
    enum class State { StreamHeader, BlockOrEnd, Output, Done, Failed };
    struct Table {
        int minLen = 0, maxLen = 0;
        int32_t limit[24] = {}, base[24] = {};
        uint16_t perm[258] = {};
        uint16_t fast[1 << 10] = {};   // next 10 bits -> (length << 9) | symbol, 0 = longer code
    };

    bool fail(const char* why);
    bool refill();
    bool bits(int n, uint32_t& v);
    bool readStreamHeader(bool first);
    bool readBlock();
    bool decodeSymbol(const Table& t, int& sym);

    Source source_;
    std::vector<uint8_t> in_;
    size_t inPos_ = 0, inLen_ = 0;
    bool inputEnded_ = false;
    uint64_t bitBuf_ = 0;
    int bitCount_ = 0;
    uint64_t consumed_ = 0;

    State state_ = State::StreamHeader;
    std::string error_;
    int blockSize100k_ = 0;
    uint32_t streamCrc_ = 0, combinedCrc_ = 0;
    // The current block.
    std::vector<uint32_t> tt_;
    uint32_t blockCrc_ = 0, crc_ = 0;
    uint32_t tPos_ = 0;
    int32_t nblock_ = 0, used_ = 0;
    int runLength_ = 0, repeat_ = 0;
    uint8_t last_ = 0;
};

// Whole-buffer convenience (tests, small files). False with 'error' set on failure.
bool decompress(const std::string& in, std::string& out, std::string* error = nullptr);

}  // namespace bz2
