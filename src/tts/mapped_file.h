// Read-only memory mapping of a whole file (the model weights are used in place, never copied).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace tts {

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool open(const std::string& path, std::string* error = nullptr);
    void close();
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    bool isOpen() const { return data_ != nullptr; }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
#if defined(_WIN32)
    void* file_ = nullptr;
    void* mapping_ = nullptr;
#endif
};

}  // namespace tts
