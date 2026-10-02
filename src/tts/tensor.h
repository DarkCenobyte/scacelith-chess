// Tensors of the TTS graph interpreter: dense, row-major, owned by a shared 64-byte aligned
// buffer, or a read-only view into the model file (initializers, never copied unless they are
// small or misaligned for generic code). Int8 weights behind a DequantizeLinear stay int8: the
// tensor then carries a QuantWeight and no float data.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tts {

enum class DType : uint8_t { None = 0, F32 = 1, U8 = 2, I8 = 3, I32 = 6, I64 = 7, Bool = 9 };

size_t dtypeSize(DType t);
const char* dtypeName(DType t);

using Dims = std::vector<int64_t>;
int64_t elementCount(const Dims& d);
// Every dimension >= 0 and at most 2^40 elements (non-zero dimensions multiplied without
// overflow): the check for shapes computed from data.
bool validDims(const Dims& d);

struct Buffer {
    void* data = nullptr;
    size_t bytes = 0;
    size_t capacity = 0;   // allocated bytes (bytes + 64, rounded up to a size class when cached)
    explicit Buffer(size_t n);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
};

// Blocks of 64 KiB and more are not returned to the system allocator when freed but kept for
// reuse (up to 128 MiB) until trimBufferCache(): a synthesis allocates and frees the same large
// activations hundreds of times, and the Windows heap hands such blocks back to the OS on every
// free (a page fault per 4 KiB page on every reuse). Synthesizer::synthesize() trims at its end.
void trimBufferCache();
size_t bufferCacheBytes();

// DequantizeLinear(q, scale, zero_point) of a constant 8-bit tensor, kept quantized.
struct QuantWeight {
    const uint8_t* q = nullptr;       // int8 (or uint8 when isUnsigned), in place in the model file
                                      // or in the buffer held by owner
    std::shared_ptr<Buffer> owner;    // null for a view into the model file
    bool isUnsigned = false;
    Dims dims;
    int axis = 0;                     // quantization axis when scale has several entries
    std::vector<float> scale;         // 1 or dims[axis] entries
    std::vector<int32_t> zeroPoint;   // same count
};

struct Tensor {
    DType type = DType::None;
    Dims dims;
    const void* data = nullptr;
    std::shared_ptr<Buffer> owner;                  // null for views into the model file
    std::shared_ptr<const QuantWeight> qweight;     // set (and data null) for quantized weights

    bool valid() const { return type != DType::None; }
    int64_t count() const { return elementCount(dims); }
    int rank() const { return int(dims.size()); }
    size_t bytes() const { return size_t(count()) * dtypeSize(type); }
    template <class T> const T* as() const { return static_cast<const T*>(data); }
    // Writable access, only for tensors this code just allocated.
    template <class T> T* mut() const { return static_cast<T*>(const_cast<void*>(data)); }

    static Tensor alloc(DType t, const Dims& d);                 // uninitialised
    static Tensor zeros(DType t, const Dims& d);
    static Tensor view(DType t, const Dims& d, const void* p);   // not owned
    // Scalar helpers (shape tensors).
    static Tensor fromInts(const std::vector<int64_t>& v);
    static Tensor scalarF32(float v);
    std::vector<int64_t> toInts() const;                         // I64 / I32 contents
    float scalarFloat() const;                                   // first element as float
};

}  // namespace tts
