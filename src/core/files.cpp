#include "files.h"
#ifdef _WIN32
#include <exception>
#include <filesystem>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace files {

#ifdef _WIN32
std::FILE* create(const char* path, bool) {
    std::filesystem::path wide;
    try {
        wide = std::filesystem::u8path(path);
    } catch (const std::exception&) {
        // Not UTF-8: bytes of the ANSI code page (GetTempPathA's, in the tests), as std::fopen takes
        // them, rather than u8path's exception.
        return std::fopen(path, "wb");
    }
    return _wfopen(wide.c_str(), L"wb");
}
#else
std::FILE* create(const char* path, bool privateFile) {
    // The permissions spelled out, as net::sys::writeFileAtomic does: never writable by other
    // users, whatever the umask (some service managers and containers start programs with 0).
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, privateFile ? 0600 : 0644);
    if (fd < 0) return nullptr;
    // The mode applies to a new file only: a private one an older version made with fopen loses
    // its group and other bits too. Regular files only, never a terminal or device behind a link.
    struct stat st;
    if (privateFile && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) fchmod(fd, 0600);
    std::FILE* f = fdopen(fd, "wb");
    if (!f) ::close(fd);
    return f;
}
#endif

}  // namespace files
