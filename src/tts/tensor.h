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

struct Buffer {
    void* data = nullptr;
    size_t bytes = 0;
    explicit Buffer(size_t n);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
};

// DequantizeLinear(q, scale, zero_point) of a constant 8-bit tensor, kept quantized.
struct QuantWeight {
    const uint8_t* q = nullptr;       // int8 (or uint8 when isUnsigned), in place in the model file
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
