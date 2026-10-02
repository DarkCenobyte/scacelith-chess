// Minimal reader of ONNX model files (protobuf wire format, onnx.proto field numbers): opset
// imports, graph inputs/outputs, nodes with their attributes, and initializers. Initializer bytes
// stored as raw_data stay in place in the caller's buffer (the memory-mapped model file in the
// game): nothing is copied except tensors stored in the typed repeated fields, which the
// exporters only use for small constants. No external data (every Supertonic file is < 2 GB).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tts {
namespace onnx {

// onnx::TensorProto::DataType values used by the runtime.
enum DataType : int { kFloat = 1, kUint8 = 2, kInt8 = 3, kInt32 = 6, kInt64 = 7, kBool = 9 };

struct TensorData {
    std::string name;
    int dataType = 0;
    std::vector<int64_t> dims;
    const uint8_t* raw = nullptr;       // raw_data in place (unaligned), or nullptr
    size_t rawSize = 0;
    std::vector<uint8_t> decoded;       // float_data / int32_data / int64_data, converted to the element type
    const uint8_t* bytes() const { return raw ? raw : decoded.data(); }
    size_t byteSize() const { return raw ? rawSize : decoded.size(); }
};

struct Attribute {
    std::string name;
    float f = 0.0f;
    int64_t i = 0;
    std::string s;
    std::vector<float> floats;
    std::vector<int64_t> ints;
    TensorData t;
};

struct NodeProto {
    std::string name, opType, domain;
    std::vector<std::string> inputs, outputs;   // "" = optional input left out
    std::vector<Attribute> attributes;
    const Attribute* attribute(const char* attrName) const;
};

struct ValueInfo {
    std::string name;
};

struct Model {
    int64_t irVersion = 0;
    int64_t opset = 0;                   // version of the default ("" / "ai.onnx") domain
    std::string producer;
    std::vector<NodeProto> nodes;        // topological order, as exported
    std::vector<TensorData> initializers;
    std::vector<ValueInfo> inputs, outputs;
};

// Parses a serialized ModelProto. The buffer must outlive the Model (raw tensors point into it).
bool parse(const uint8_t* data, size_t size, Model& out, std::string* error);

}  // namespace onnx
}  // namespace tts
