#include "tensor.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
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

bool validDims(const Dims& d) {
    int64_t n = 1;
    for (int64_t x : d)
        if (x < 0 || (x && (__builtin_mul_overflow(n, x, &n) || n > (int64_t(1) << 40)))) return false;
    return true;
}

namespace {

constexpr size_t kCacheMin = size_t(64) << 10;
constexpr size_t kCacheMax = size_t(128) << 20;

struct BlockCache {
    std::mutex mutex;
    std::multimap<size_t, void*> blocks;   // capacity -> block
    size_t bytes = 0;
};

// Never destroyed: buffers owned by static objects may be freed after it would be.
BlockCache& cache() {
    static BlockCache* c = new BlockCache;
    return *c;
}

// Size classes of cached blocks: eight per power of two (at most 12.5 % unused).
size_t sizeClass(size_t n) {
    size_t top = size_t(1) << (63 - __builtin_clzll(static_cast<unsigned long long>(n)));
    size_t step = std::max<size_t>(top / 8, 4096);
    return (n + step - 1) / step * step;
}

}  // namespace

Buffer::Buffer(size_t n) : bytes(n), capacity(n + 64) {
    if (n > SIZE_MAX / 2) throw std::bad_alloc();   // a wrapped size: never a tiny block
    // 64-byte aligned, rounded up so SIMD tails may read a full register past the end.
    if (capacity >= kCacheMin) {
        capacity = sizeClass(capacity);
        BlockCache& c = cache();
        std::lock_guard<std::mutex> lock(c.mutex);
        auto it = c.blocks.find(capacity);
        if (it != c.blocks.end()) {
            data = it->second;
            c.bytes -= capacity;
            c.blocks.erase(it);
            return;
        }
    }
    data = ::operator new(capacity, std::align_val_t(64));
}

Buffer::~Buffer() {
    if (capacity >= kCacheMin) {
        BlockCache& c = cache();
        std::lock_guard<std::mutex> lock(c.mutex);
        if (c.bytes + capacity <= kCacheMax) {
            c.blocks.emplace(capacity, data);
            c.bytes += capacity;
            return;
        }
    }
    ::operator delete(data, std::align_val_t(64));
}

void trimBufferCache() {
    std::multimap<size_t, void*> blocks;
    {
        BlockCache& c = cache();
        std::lock_guard<std::mutex> lock(c.mutex);
        blocks.swap(c.blocks);
        c.bytes = 0;
    }
    for (auto& b : blocks) ::operator delete(b.second, std::align_val_t(64));
}

size_t bufferCacheBytes() {
    BlockCache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    return c.bytes;
}

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
