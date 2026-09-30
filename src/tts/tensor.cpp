#include "tensor.h"
#include <cstdlib>
#include <cstring>
#include <new>

namespace tts {

size_t dtypeSize(DType t) {
    switch (t) {
    case DType::F32: case DType::I32: return 4;
    case DType::U8: case DType::I8: case DType::Bool: return 1;
    case DType::I64: return 8;
    default: return 0;
    }
}

const char* dtypeName(DType t) {
    switch (t) {
    case DType::F32: return "float";
    case DType::U8: return "uint8";
    case DType::I8: return "int8";
    case DType::I32: return "int32";
    case DType::I64: return "int64";
    case DType::Bool: return "bool";
    default: return "none";
    }
}

int64_t elementCount(const Dims& d) {
    int64_t n = 1;
    for (int64_t x : d) n *= x;
    return n;
}

Buffer::Buffer(size_t n) : bytes(n) {
    // 64-byte aligned, rounded up so SIMD tails may read a full register past the end.
    data = ::operator new(n + 64, std::align_val_t(64));
}

Buffer::~Buffer() { ::operator delete(data, std::align_val_t(64)); }

Tensor Tensor::alloc(DType t, const Dims& d) {
    Tensor r;
    r.type = t;
    r.dims = d;
    r.owner = std::make_shared<Buffer>(size_t(elementCount(d)) * dtypeSize(t));
    r.data = r.owner->data;
    return r;
}

Tensor Tensor::zeros(DType t, const Dims& d) {
    Tensor r = alloc(t, d);
    std::memset(r.owner->data, 0, r.owner->bytes);
    return r;
}

Tensor Tensor::view(DType t, const Dims& d, const void* p) {
    Tensor r;
    r.type = t;
    r.dims = d;
    r.data = p;
    return r;
}

Tensor Tensor::fromInts(const std::vector<int64_t>& v) {
    Tensor r = alloc(DType::I64, {int64_t(v.size())});
    if (!v.empty()) std::memcpy(r.owner->data, v.data(), v.size() * 8);
    return r;
}

Tensor Tensor::scalarF32(float v) {
    Tensor r = alloc(DType::F32, {});
    *r.mut<float>() = v;
    return r;
}

std::vector<int64_t> Tensor::toInts() const {
    std::vector<int64_t> v(static_cast<size_t>(count()));
    for (size_t i = 0; i < v.size(); ++i) {
        if (type == DType::I64) {
            int64_t x;
            std::memcpy(&x, static_cast<const char*>(data) + i * 8, 8);
            v[i] = x;
        } else if (type == DType::I32) {
            int32_t x;
            std::memcpy(&x, static_cast<const char*>(data) + i * 4, 4);
            v[i] = x;
        } else {
            v[i] = 0;
        }
    }
    return v;
}

float Tensor::scalarFloat() const {
    if (!data || count() < 1) return 0.0f;
    switch (type) {
    case DType::F32: {
        float f;
        std::memcpy(&f, data, 4);
        return f;
    }
    case DType::I64: return float(toInts()[0]);
    case DType::I32: return float(toInts()[0]);
    case DType::U8: return float(*static_cast<const uint8_t*>(data));
    case DType::I8: return float(*static_cast<const int8_t*>(data));
    case DType::Bool: return *static_cast<const uint8_t*>(data) ? 1.0f : 0.0f;
    default: return 0.0f;
    }
}

}  // namespace tts
