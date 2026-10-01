// Reading a tar archive as a stream (the release archive of the coach's voice model, after
// bz2::Decoder): POSIX ustar headers (with the prefix field), the GNU long name entries ('L',
// 'K') and pax extended headers ('x' path and size, 'g' skipped), octal or GNU base-256 sizes,
// header checksums. Entries come one after the other; the data of an entry is read in pieces, and
// whatever is not read is skipped by next(). Nothing is written to disk here: the caller decides
// what to extract (safePath() and Entry::regular() are its checks).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace tar {

struct Entry {
    std::string path;          // as stored ("folder/file", directories end with '/')
    char type = '0';           // typeflag: '0' file, '5' directory, '2' symbolic link, '1' hard link...
    uint64_t size = 0;         // bytes of data
    std::string linkTarget;    // links only
    // A regular file ('0', NUL from old tars, '7' contiguous file): the only kind worth extracting.
    bool regular() const { return type == '0' || type == '\0' || type == '7'; }
};

// A relative path that stays inside the folder it is extracted to: no root or drive, no
// backslash, no "." or ".." component, no empty component (a trailing '/' is allowed).
bool safePath(const std::string& path);

class Reader {
public:
    // Pulls archive bytes: the count read into buf (at most n), 0 at the end, -1 on an error.
    using Source = std::function<long(uint8_t* buf, size_t n)>;
    explicit Reader(Source source);

    // The next entry: true with 'e' filled; false at the end of the archive (error() empty) or
    // on an error (bad header, checksum, truncated archive).
    bool next(Entry& e);
    // Data of the current entry: the count read (at most n), 0 at its end, -1 on an error.
    long read(uint8_t* buf, size_t n);
    const std::string& error() const { return error_; }

private:
    bool readFully(uint8_t* buf, size_t n);   // false (error set) when fewer bytes came
    bool skip(uint64_t n);
    bool readText(uint64_t size, std::string& out);   // the data of a long name or pax entry

    Source source_;
    std::string error_;
    uint64_t left_ = 0;        // data bytes of the current entry not read yet
    uint64_t padding_ = 0;     // then the padding to the next 512-byte block
    bool ended_ = false;
};

}  // namespace tar
