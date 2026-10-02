// ONNX graph interpreter of the TTS runtime: loads a model (in place), folds the constant
// subgraphs, fuses a few hot patterns (exact-erf GELU, channel LayerNorm, quantized MatMul
// epilogues, fake-quantized activations) and runs the nodes in the exported topological order with dynamic shapes.
//
// Supported operators (opset 19, the set used by the Supertonic 3 graphs, see
// research-supertonic.md 5.3): Add Sub Mul Div Pow Equal Where Cast Clip Relu PRelu Erf Exp Sin
// Cos Tanh Softplus Reciprocal Concat Split Slice Gather Reshape Transpose Unsqueeze Squeeze Shape
// ConstantOfShape Expand Tile Pad(constant, edge) Softmax LayerNormalization BatchNormalization
// ReduceSum MatMul Gemm Conv(1-D) MatMulInteger DynamicQuantizeLinear QuantizeLinear
// DequantizeLinear Constant Identity.
#pragma once
#include "kernels.h"
#include "onnx.h"
#include "tensor.h"
#include <atomic>
#include <string>
#include <vector>

namespace tts {

class ThreadPool;

enum class Op : uint8_t {
    Add, Sub, Mul, Div, Pow, Equal, Where, Cast, Clip, Relu, PRelu, Erf, Exp, Sin, Cos, Tanh, Softplus, Reciprocal,
    Concat, Split, Slice, Gather, Reshape, Transpose, Unsqueeze, Squeeze, Shape, ConstantOfShape, Expand, Tile, Pad,
    Softmax, LayerNorm, BatchNorm, ReduceSum, MatMul, Gemm, Conv, MatMulInteger, DynamicQuantize, Quantize,
    Dequantize, Constant, Identity,
    // Fused by the loader.
    Gelu,               // Div(x, sqrt 2) -> Erf -> Add 1 -> Mul x -> Mul 0.5
    LayerNormChannels,  // Transpose(0,2,1) -> LayerNormalization(-1) -> Transpose(0,2,1) on [B, C, L]
    MatMulIntegerScaled,// MatMulInteger -> Cast(float) -> Mul(scale) [-> Add(bias)]
    QuantDequant,       // QuantizeLinear -> DequantizeLinear with the same per-tensor uint8 parameters
    Count
};
const char* opName(Op op);

// Development profiling: seconds spent per operator (Op::Count entries) by every Session::run
// while enabled, all sessions and threads together (not synchronised: one synthesis at a time).
void setProfiling(bool on);
const double* profileSeconds();
void resetProfile();
// The same per node ("graph operator name [output dims]: ms"), slowest first.
std::string profileReport(size_t maxLines);

struct Node {
    Op op = Op::Identity;
    std::string name;
    std::vector<int> in, out;          // value indices, -1 for an absent optional input
    // Attributes (meaning per operator, see graph.cpp).
    int64_t axis = 0, i0 = 0, i1 = 0;
    float f0 = 0.0f, f1 = 0.0f;
    std::vector<int64_t> ints, ints2, ints3;
    std::string mode;
    Tensor value;                      // Constant / ConstantOfShape
    // Set by the loader.
    bool invariant = false;            // depends only on constants and invariant inputs
    std::vector<int32_t> bSums;        // MatMulInteger with a constant int8 B: the sums of its columns (VNNI)
};

struct Value {
    std::string name;
    bool isConst = false;
    Tensor constant;
    int producer = -1;
    std::vector<int> consumers;        // node indices (a node consuming twice is listed twice)
    int lastUse = -1;
    bool isOutput = false;
    bool invariant = false;
    bool keep = false;                 // invariant value kept between runs of a Session
};

struct ExecContext {
    const kern::Table* k = nullptr;
    ThreadPool* pool = nullptr;        // may be null (single thread)
};

// Runs one node; 'in' has node.in.size() entries (nullptr for absent inputs).
bool execNode(const Node& n, const Tensor* const* in, Tensor* out, const ExecContext& ctx, std::string* error);

class Graph {
public:
    // 'data' must stay valid (mapped) for the lifetime of the Graph. 'invariantInputs' names the
    // inputs that stay the same across the runs of one Session (see Session).
    bool load(const uint8_t* data, size_t size, const std::string& label,
              const std::vector<std::string>& invariantInputs, const kern::Table& k, std::string* error);
    const std::string& label() const { return label_; }
    int inputIndex(const char* name) const;
    int outputIndex(const char* name) const;
    size_t inputCount() const { return inputs_.size(); }
    size_t outputCount() const { return outputs_.size(); }
    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Value>& values() const { return values_; }
    const std::vector<int>& inputs() const { return inputs_; }
    const std::vector<int>& outputs() const { return outputs_; }
    size_t inPlaceBytes() const { return inPlaceBytes_; }    // initializer bytes used in place
    size_t copiedBytes() const { return copiedBytes_; }      // initializer bytes copied (small or misaligned)
    int fusedCount() const { return fused_; }

private:
    bool foldConstants(const kern::Table& k, std::string* error);
    void fusePatterns();
    void finish(const kern::Table& k);
    int addValue(const std::string& name);

    std::string label_;
    onnx::Model model_;
    std::vector<Value> values_;
    std::vector<Node> nodes_;
    std::vector<int> inputs_, outputs_;
    size_t inPlaceBytes_ = 0, copiedBytes_ = 0;
    int fused_ = 0;
};

// Execution state of one graph. Values computed only from constants and the invariant inputs
// are computed on the first run() and kept until clearCache() (the flow-matching loop runs the
// vector estimator several times with the same text and style).
class Session {
public:
    explicit Session(const Graph& g) : g_(g) {}
    void setInput(int index, Tensor t);
    bool run(const ExecContext& ctx, std::string* error, const std::atomic<bool>* cancel = nullptr);
    const Tensor& output(int index) const;
    void clearCache();
    // Debug: keep every value (for per-node comparisons); off by default.
    void setKeepAll(bool keep) { keepAll_ = keep; }
    const Tensor& value(int v) const { return slots_[size_t(v)]; }

private:
    const Graph& g_;
    std::vector<Tensor> slots_;
    bool cached_ = false;
    bool keepAll_ = false;
};

}  // namespace tts
