#include "mapped_file.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tts {

MappedFile::~MappedFile() { close(); }

#if defined(_WIN32)

bool MappedFile::open(const std::string& path, std::string* error) {
    close();
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(size_t(n > 0 ? n : 1), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], n);
    HANDLE f = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart == 0) {
        CloseHandle(f);
        if (error) *error = "empty or unreadable file " + path;
        return false;
    }
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* p = m ? MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (!p) {
        if (m) CloseHandle(m);
        CloseHandle(f);
        if (error) *error = "cannot map " + path;
        return false;
    }
    file_ = f;
    mapping_ = m;
    data_ = static_cast<const uint8_t*>(p);
    size_ = size_t(sz.QuadPart);
    return true;
}

void MappedFile::close() {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
    data_ = nullptr;
    mapping_ = file_ = nullptr;
    size_ = 0;
}

#else

bool MappedFile::open(const std::string& path, std::string* error) {
    close();
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        if (error) *error = "empty or unreadable file " + path;
        return false;
    }
    void* p = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        if (error) *error = "cannot map " + path;
        return false;
    }
    data_ = static_cast<const uint8_t*>(p);
    size_ = size_t(st.st_size);
    return true;
}

void MappedFile::close() {
    if (data_) munmap(const_cast<uint8_t*>(data_), size_);
    data_ = nullptr;
    size_ = 0;
}

#endif

}  // namespace tts
