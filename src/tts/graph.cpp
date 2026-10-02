#include "graph.h"
#include "core/log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>

namespace tts {
namespace {

struct OpEntry {
    const char* name;
    Op op;
};

const OpEntry kOps[] = {
    {"Add", Op::Add}, {"Sub", Op::Sub}, {"Mul", Op::Mul}, {"Div", Op::Div}, {"Pow", Op::Pow},
    {"Equal", Op::Equal}, {"Where", Op::Where}, {"Cast", Op::Cast}, {"Clip", Op::Clip}, {"Relu", Op::Relu},
    {"PRelu", Op::PRelu}, {"Erf", Op::Erf}, {"Exp", Op::Exp}, {"Sin", Op::Sin}, {"Cos", Op::Cos},
    {"Tanh", Op::Tanh}, {"Softplus", Op::Softplus}, {"Reciprocal", Op::Reciprocal}, {"Concat", Op::Concat},
    {"Split", Op::Split}, {"Slice", Op::Slice}, {"Gather", Op::Gather}, {"Reshape", Op::Reshape},
    {"Transpose", Op::Transpose}, {"Unsqueeze", Op::Unsqueeze}, {"Squeeze", Op::Squeeze}, {"Shape", Op::Shape},
    {"ConstantOfShape", Op::ConstantOfShape}, {"Expand", Op::Expand}, {"Tile", Op::Tile}, {"Pad", Op::Pad},
    {"Softmax", Op::Softmax}, {"LayerNormalization", Op::LayerNorm}, {"BatchNormalization", Op::BatchNorm},
    {"ReduceSum", Op::ReduceSum}, {"MatMul", Op::MatMul}, {"Gemm", Op::Gemm}, {"Conv", Op::Conv},
    {"MatMulInteger", Op::MatMulInteger}, {"DynamicQuantizeLinear", Op::DynamicQuantize},
    {"QuantizeLinear", Op::Quantize}, {"DequantizeLinear", Op::Dequantize}, {"Constant", Op::Constant},
    {"Identity", Op::Identity},
};

DType dtypeOf(int onnxType) {
    switch (onnxType) {
    case onnx::kFloat: return DType::F32;
    case onnx::kUint8: return DType::U8;
    case onnx::kInt8: return DType::I8;
    case onnx::kInt32: return DType::I32;
    case onnx::kInt64: return DType::I64;
    case onnx::kBool: return DType::Bool;
    default: return DType::None;
    }
}

// Tensor view of ONNX tensor data in place, or an aligned copy.
Tensor tensorOf(const onnx::TensorData& t, bool copy) {
    DType type = dtypeOf(t.dataType);
    if (type == DType::None) return Tensor();
    if (!copy && t.raw) return Tensor::view(type, t.dims, t.bytes());
    Tensor r = Tensor::alloc(type, t.dims);
    if (t.byteSize()) std::memcpy(r.owner->data, t.bytes(), t.byteSize());
    return r;
}

float constScalar(const Value& v, bool* ok) {
    if (!v.isConst || v.constant.count() != 1 || v.constant.type != DType::F32) {
        *ok = false;
        return 0.0f;
    }
    return v.constant.scalarFloat();
}

std::atomic<bool> g_profiling{false};
double g_profile[int(Op::Count)] = {};
std::map<std::string, double> g_profileNodes;

}  // namespace

void setProfiling(bool on) { g_profiling = on; }
const double* profileSeconds() { return g_profile; }
void resetProfile() {
    for (double& d : g_profile) d = 0.0;
    g_profileNodes.clear();
}

std::string profileReport(size_t maxLines) {
    std::vector<std::pair<double, std::string>> v;
    for (const auto& e : g_profileNodes) v.push_back({e.second, e.first});
    std::sort(v.rbegin(), v.rend());
    std::string r;
    for (size_t i = 0; i < v.size() && i < maxLines; ++i) {
        char ms[32];
        std::snprintf(ms, sizeof ms, ": %.2f ms\n", v[i].first * 1e3);
        r += v[i].second + ms;
    }
    return r;
}

const char* opName(Op op) {
    for (const OpEntry& e : kOps)
        if (e.op == op) return e.name;
    switch (op) {
    case Op::Gelu: return "Gelu";
    case Op::LayerNormChannels: return "LayerNormChannels";
    case Op::MatMulIntegerScaled: return "MatMulIntegerScaled";
    case Op::QuantDequant: return "QuantDequant";
    default: return "?";
    }
}

int Graph::addValue(const std::string& name) {
    values_.emplace_back();
    values_.back().name = name;
    return int(values_.size()) - 1;
}

int Graph::inputIndex(const char* name) const {
    for (size_t i = 0; i < inputs_.size(); ++i)
        if (values_[size_t(inputs_[i])].name == name) return int(i);
    return -1;
}

int Graph::outputIndex(const char* name) const {
    for (size_t i = 0; i < outputs_.size(); ++i)
        if (values_[size_t(outputs_[i])].name == name) return int(i);
    return -1;
}

bool Graph::load(const uint8_t* data, size_t size, const std::string& label,
                 const std::vector<std::string>& invariantInputs, const kern::Table& k, std::string* error) {
    label_ = label;
    if (!onnx::parse(data, size, model_, error)) return false;
    if (model_.opset < 13) {
        if (error) *error = label + ": opset " + std::to_string(model_.opset) + " not supported (13 or later)";
        return false;
    }
    std::unordered_map<std::string, int> byName;

    // Which initializers are read only through kernels that accept any alignment (GEMM packing,
    // Gather rows): those stay in place even when misaligned. Everything else that is misaligned
    // or small is copied, so generic loops always see naturally aligned data.
    std::unordered_map<std::string, bool> kernelOnly;
    for (const onnx::NodeProto& n : model_.nodes)
        for (size_t i = 0; i < n.inputs.size(); ++i) {
            bool ok = (n.opType == "Conv" && i == 1) || (n.opType == "MatMul") || (n.opType == "Gather" && i == 0) ||
                      (n.opType == "MatMulInteger" && i == 1) || (n.opType == "DequantizeLinear" && i == 0);
            auto it = kernelOnly.find(n.inputs[i]);
            if (it == kernelOnly.end()) kernelOnly[n.inputs[i]] = ok;
            else it->second = it->second && ok;
        }
    for (const onnx::TensorData& t : model_.initializers) {
        int v = addValue(t.name);
        byName[t.name] = v;
        Value& val = values_[size_t(v)];
        val.isConst = true;
        auto it = kernelOnly.find(t.name);
        bool inPlaceOk = it != kernelOnly.end() && it->second && t.byteSize() >= 65536;
        size_t es = dtypeSize(dtypeOf(t.dataType));
        bool aligned = es && reinterpret_cast<uintptr_t>(t.bytes()) % es == 0;
        bool copy = !t.raw || t.byteSize() < 4096 || (!aligned && !inPlaceOk);
        val.constant = tensorOf(t, copy);
        (copy ? copiedBytes_ : inPlaceBytes_) += t.byteSize();
        if (!val.constant.valid()) {
            if (error) *error = label + ": initializer " + t.name + " has an unsupported type";
            return false;
        }
    }
    for (const onnx::ValueInfo& vi : model_.inputs) {
        if (byName.count(vi.name)) continue;   // an initializer listed as input
        int v = addValue(vi.name);
        byName[vi.name] = v;
        inputs_.push_back(v);
        for (const std::string& inv : invariantInputs)
            if (inv == vi.name) values_[size_t(v)].invariant = true;
    }

    for (const onnx::NodeProto& pn : model_.nodes) {
        Node n;
        n.name = pn.name;
        bool found = false;
        if (pn.domain.empty() || pn.domain == "ai.onnx")
            for (const OpEntry& e : kOps)
                if (pn.opType == e.name) {
                    n.op = e.op;
                    found = true;
                }
        if (!found) {
            if (error) *error = label + ": unsupported operator " + pn.domain + ":" + pn.opType + " (" + pn.name + ")";
            return false;
        }
        auto attrI = [&pn](const char* a, int64_t def) {
            const onnx::Attribute* at = pn.attribute(a);
            return at ? at->i : def;
        };
        auto attrF = [&pn](const char* a, float def) {
            const onnx::Attribute* at = pn.attribute(a);
            return at ? at->f : def;
        };
        auto attrInts = [&pn](const char* a) {
            const onnx::Attribute* at = pn.attribute(a);
            return at ? at->ints : std::vector<int64_t>();
        };
        switch (n.op) {
        case Op::Shape:
            n.i0 = attrI("start", 0);
            n.i1 = attrI("end", std::numeric_limits<int64_t>::max());
            break;
        case Op::Gather: case Op::Concat: n.axis = attrI("axis", 0); break;
        case Op::Split:
            n.axis = attrI("axis", 0);
            n.ints = attrInts("split");
            n.i0 = attrI("num_outputs", 0);
            break;
        case Op::Transpose: n.ints = attrInts("perm"); break;
        case Op::Softmax: n.axis = attrI("axis", -1); break;
        case Op::LayerNorm:
            n.axis = attrI("axis", -1);
            n.f0 = attrF("epsilon", 1e-5f);
            break;
        case Op::BatchNorm: n.f0 = attrF("epsilon", 1e-5f); break;
        case Op::ReduceSum:
            n.i0 = attrI("keepdims", 1);
            n.i1 = attrI("noop_with_empty_axes", 0);
            break;
        case Op::Cast: n.i0 = attrI("to", 1); break;
        case Op::Conv: {
            n.ints = attrInts("dilations");
            n.ints2 = attrInts("pads");
            n.ints3 = attrInts("strides");
            n.i0 = attrI("group", 1);
            const onnx::Attribute* ap = pn.attribute("auto_pad");
            if (ap && !ap->s.empty() && ap->s != "NOTSET") {
                if (error) *error = label + ": Conv auto_pad " + ap->s + " not supported";
                return false;
            }
            break;
        }
        case Op::Gemm:
            n.f0 = attrF("alpha", 1.0f);
            n.f1 = attrF("beta", 1.0f);
            n.i0 = attrI("transA", 0);
            n.i1 = attrI("transB", 0);
            break;
        case Op::Pad: {
            const onnx::Attribute* m = pn.attribute("mode");
            n.mode = m ? m->s : "constant";
            if (n.mode != "constant" && n.mode != "edge") {
                if (error) *error = label + ": Pad mode " + n.mode + " not supported";
                return false;
            }
            break;
        }
        case Op::Quantize: case Op::Dequantize:
            n.axis = attrI("axis", 1);
            n.i0 = attrI("saturate", 1);
            break;
        case Op::Reshape: n.i0 = attrI("allowzero", 0); break;
        case Op::ConstantOfShape: {
            const onnx::Attribute* v = pn.attribute("value");
            n.value = v ? tensorOf(v->t, true) : Tensor::scalarF32(0.0f);
            break;
        }
        case Op::Constant: {
            const onnx::Attribute* v = pn.attribute("value");
            if (v) {
                n.value = tensorOf(v->t, true);
            } else if ((v = pn.attribute("value_float"))) {
                n.value = Tensor::scalarF32(v->f);
            } else if ((v = pn.attribute("value_int"))) {
                n.value = Tensor::fromInts({v->i});
                n.value.dims.clear();
            } else if ((v = pn.attribute("value_ints"))) {
                n.value = Tensor::fromInts(v->ints);
            } else if ((v = pn.attribute("value_floats"))) {
                n.value = Tensor::alloc(DType::F32, {int64_t(v->floats.size())});
                if (!v->floats.empty()) std::memcpy(n.value.owner->data, v->floats.data(), v->floats.size() * 4);
            }
            if (!n.value.valid()) {
                if (error) *error = label + ": Constant " + pn.name + " has no supported value";
                return false;
            }
            break;
        }
        default: break;
        }
        for (const std::string& s : pn.inputs) {
            if (s.empty()) {
                n.in.push_back(-1);
                continue;
            }
            auto it = byName.find(s);
            if (it == byName.end()) {
                if (error) *error = label + ": node " + pn.name + " reads undefined value " + s;
                return false;
            }
            n.in.push_back(it->second);
        }
        for (const std::string& s : pn.outputs) {
            int v = addValue(s);
            byName[s] = v;
            values_[size_t(v)].producer = int(nodes_.size());
            n.out.push_back(v);
        }
        nodes_.push_back(std::move(n));
    }
    for (const onnx::ValueInfo& vi : model_.outputs) {
        auto it = byName.find(vi.name);
        if (it == byName.end()) {
            if (error) *error = label + ": output " + vi.name + " is never produced";
            return false;
        }
        outputs_.push_back(it->second);
        values_[size_t(it->second)].isOutput = true;
    }
    if (!foldConstants(k, error)) return false;
    fusePatterns();
    finish(k);
    return true;
}

// Evaluates, once, every node whose inputs are all constants (shape arithmetic on constant
// shapes, Constant nodes, input-independent subgraphs such as the text encoder's Tanh style keys).
// DequantizeLinear of a large 8-bit initializer becomes a QuantWeight instead of float data.
bool Graph::foldConstants(const kern::Table& k, std::string* error) {
    ExecContext ctx;
    ctx.k = &k;
    std::vector<Node> kept;
    kept.reserve(nodes_.size());
    for (Node& n : nodes_) {
        bool allConst = true;
        for (int v : n.in)
            if (v >= 0 && !values_[size_t(v)].isConst) allConst = false;
        if (!allConst) {
            kept.push_back(std::move(n));
            continue;
        }
        if (n.op == Op::Dequantize) {
            const Tensor& x = values_[size_t(n.in[0])].constant;
            if ((x.type == DType::I8 || x.type == DType::U8) && x.count() >= 4096) {
                auto q = std::make_shared<QuantWeight>();
                q->q = x.as<uint8_t>();
                q->isUnsigned = x.type == DType::U8;
                q->dims = x.dims;
                const Tensor& s = values_[size_t(n.in[1])].constant;
                q->scale.resize(size_t(s.count()));
                std::memcpy(q->scale.data(), s.data, q->scale.size() * 4);
                q->zeroPoint.assign(q->scale.size(), 0);
                if (n.in.size() > 2 && n.in[2] >= 0) {
                    const Tensor& z = values_[size_t(n.in[2])].constant;
                    for (size_t i = 0; i < q->zeroPoint.size() && i < size_t(z.count()); ++i)
                        q->zeroPoint[i] = z.type == DType::U8 ? int(z.as<uint8_t>()[i]) : int(z.as<int8_t>()[i]);
                }
                int64_t axis = n.axis < 0 ? n.axis + x.rank() : n.axis;
                q->axis = int(axis);
                if (q->scale.size() != 1 && (axis < 0 || axis >= x.rank() || x.dims[size_t(axis)] != int64_t(q->scale.size()))) {
                    if (error) *error = label_ + ": " + n.name + ": bad per-axis quantization";
                    return false;
                }
                Value& out = values_[size_t(n.out[0])];
                out.isConst = true;
                out.constant.type = DType::F32;
                out.constant.dims = x.dims;
                out.constant.qweight = q;
                continue;
            }
        }
        std::vector<const Tensor*> ins;
        for (int v : n.in) ins.push_back(v >= 0 ? &values_[size_t(v)].constant : nullptr);
        std::vector<Tensor> outs(n.out.size());
        std::string err;
        if (!execNode(n, ins.data(), outs.data(), ctx, &err)) {
            if (error) *error = label_ + ": folding " + n.name + ": " + err;
            return false;
        }
        for (size_t i = 0; i < n.out.size(); ++i) {
            Value& out = values_[size_t(n.out[i])];
            out.isConst = true;
            out.constant = std::move(outs[i]);
            copiedBytes_ += out.constant.bytes();
        }
    }
    nodes_ = std::move(kept);
    return true;
}

// Pattern fusion. Every intermediate value of a pattern must have exactly one consumer and not
// be a graph output, so the rewritten graph computes the same outputs. The fused node takes the
// place of the last node of its pattern, where all of its inputs are known to be computed.
void Graph::fusePatterns() {
    // Consumers of every value in the current node list.
    std::vector<std::vector<int>> cons(values_.size());
    for (size_t i = 0; i < nodes_.size(); ++i)
        for (int v : nodes_[i].in)
            if (v >= 0) cons[size_t(v)].push_back(int(i));
    auto single = [&](int v) -> int {
        if (v < 0 || values_[size_t(v)].isOutput || cons[size_t(v)].size() != 1) return -1;
        return cons[size_t(v)][0];
    };
    auto constIs = [&](int v, float want) {
        bool ok = true;
        float f = v >= 0 ? constScalar(values_[size_t(v)], &ok) : 0.0f;
        return v >= 0 && ok && f == want;
    };
    std::vector<bool> dead(nodes_.size(), false);

    for (size_t i = 0; i < nodes_.size(); ++i) {
        Node& n = nodes_[i];
        if (dead[i]) continue;
        // GELU: Div(x, 1.41421354) -> Erf -> Add(., 1) -> Mul(x, .) -> Mul(., 0.5)
        if (n.op == Op::Div && n.in.size() == 2 && constIs(n.in[1], 1.41421353816986083984375f)) {
            int x = n.in[0];
            int e = single(n.out[0]);
            if (e < 0 || nodes_[size_t(e)].op != Op::Erf) continue;
            int a = single(nodes_[size_t(e)].out[0]);
            if (a < 0 || nodes_[size_t(a)].op != Op::Add) continue;
            const Node& add = nodes_[size_t(a)];
            int other = add.in[0] == nodes_[size_t(e)].out[0] ? add.in[1] : add.in[0];
            if (!constIs(other, 1.0f)) continue;
            int m1 = single(add.out[0]);
            if (m1 < 0 || nodes_[size_t(m1)].op != Op::Mul) continue;
            const Node& mul1 = nodes_[size_t(m1)];
            int o1 = mul1.in[0] == add.out[0] ? mul1.in[1] : mul1.in[0];
            if (o1 != x) continue;
            int m2 = single(mul1.out[0]);
            if (m2 < 0 || nodes_[size_t(m2)].op != Op::Mul) continue;
            const Node& mul2 = nodes_[size_t(m2)];
            int o2 = mul2.in[0] == mul1.out[0] ? mul2.in[1] : mul2.in[0];
            if (!constIs(o2, 0.5f)) continue;
            Node g;
            g.op = Op::Gelu;
            g.name = n.name + "+gelu";
            g.in = {x};
            g.out = {mul2.out[0]};
            dead[i] = dead[size_t(e)] = dead[size_t(a)] = dead[size_t(m1)] = true;
            nodes_[size_t(m2)] = std::move(g);
            ++fused_;
            continue;
        }
        // Channel LayerNorm: Transpose(0,2,1) -> LayerNormalization(axis -1) -> Transpose(0,2,1)
        if (n.op == Op::Transpose && n.ints == std::vector<int64_t>{0, 2, 1}) {
            int l = single(n.out[0]);
            if (l < 0 || nodes_[size_t(l)].op != Op::LayerNorm || nodes_[size_t(l)].in[0] != n.out[0]) continue;
            const Node& ln = nodes_[size_t(l)];
            if (ln.axis != -1 && ln.axis != 2) continue;
            if (ln.out.size() > 1) {
                bool extraUsed = false;
                for (size_t o = 1; o < ln.out.size(); ++o)
                    if (!cons[size_t(ln.out[o])].empty() || values_[size_t(ln.out[o])].isOutput) extraUsed = true;
                if (extraUsed) continue;
            }
            int t2 = single(ln.out[0]);
            if (t2 < 0 || nodes_[size_t(t2)].op != Op::Transpose ||
                nodes_[size_t(t2)].ints != std::vector<int64_t>{0, 2, 1})
                continue;
            Node f;
            f.op = Op::LayerNormChannels;
            f.name = ln.name + "+channels";
            f.in = {n.in[0], ln.in[1], ln.in.size() > 2 ? ln.in[2] : -1};
            f.out = {nodes_[size_t(t2)].out[0]};
            f.f0 = ln.f0;
            dead[i] = dead[size_t(l)] = true;
            nodes_[size_t(t2)] = std::move(f);
            ++fused_;
            continue;
        }
        // Dynamic-quantized MatMul: MatMulInteger -> Cast(float) -> Mul(., scale) [-> Add(bias, .)]
        if (n.op == Op::MatMulInteger) {
            int c = single(n.out[0]);
            if (c < 0 || nodes_[size_t(c)].op != Op::Cast || nodes_[size_t(c)].i0 != onnx::kFloat) continue;
            int m = single(nodes_[size_t(c)].out[0]);
            if (m < 0 || nodes_[size_t(m)].op != Op::Mul) continue;
            const Node& mul = nodes_[size_t(m)];
            int scale = mul.in[0] == nodes_[size_t(c)].out[0] ? mul.in[1] : mul.in[0];
            int outV = mul.out[0];
            int bias = -1, last = m;
            int a = single(outV);
            if (a >= 0 && nodes_[size_t(a)].op == Op::Add) {
                const Node& add = nodes_[size_t(a)];
                int b = add.in[0] == outV ? add.in[1] : add.in[0];
                const Value& bv = values_[size_t(b)];
                if (bv.isConst && bv.constant.type == DType::F32 && bv.constant.rank() == 1 && !bv.constant.qweight) {
                    bias = b;
                    outV = add.out[0];
                    last = a;
                }
            }
            Node f;
            f.op = Op::MatMulIntegerScaled;
            f.name = n.name + "+scaled";
            f.in = n.in;
            while (f.in.size() < 4) f.in.push_back(-1);
            f.in.push_back(scale);
            f.in.push_back(bias);
            f.out = {outV};
            dead[i] = dead[size_t(c)] = true;
            if (last != m) dead[size_t(m)] = true;
            nodes_[size_t(last)] = std::move(f);
            ++fused_;
            continue;
        }
        // Fake-quantized activation (the vocoder's static QDQ): QuantizeLinear -> DequantizeLinear
        // with the same per-tensor scale and uint8 zero point.
        if (n.op == Op::Quantize && n.in.size() == 3 && n.out.size() == 1) {
            int d = single(n.out[0]);
            if (d < 0 || nodes_[size_t(d)].op != Op::Dequantize) continue;
            const Node& dq = nodes_[size_t(d)];
            if (dq.in.size() != 3 || dq.in[0] != n.out[0]) continue;
            auto scalarConst = [&](int v, DType t) -> const Tensor* {
                if (v < 0 || !values_[size_t(v)].isConst) return nullptr;
                const Tensor& c = values_[size_t(v)].constant;
                return c.type == t && c.count() == 1 && !c.qweight ? &c : nullptr;
            };
            const Tensor *s1 = scalarConst(n.in[1], DType::F32), *s2 = scalarConst(dq.in[1], DType::F32);
            const Tensor *z1 = scalarConst(n.in[2], DType::U8), *z2 = scalarConst(dq.in[2], DType::U8);
            if (!s1 || !s2 || !z1 || !z2) continue;
            if (std::memcmp(s1->data, s2->data, 4) != 0 || *z1->as<uint8_t>() != *z2->as<uint8_t>()) continue;
            Node f;
            f.op = Op::QuantDequant;
            f.name = n.name + "+dq";
            f.in = {n.in[0], n.in[1], n.in[2]};
            f.out = {dq.out[0]};
            dead[i] = true;
            nodes_[size_t(d)] = std::move(f);
            ++fused_;
            continue;
        }
    }
    std::vector<Node> kept;
    kept.reserve(nodes_.size());
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (!dead[i]) kept.push_back(std::move(nodes_[i]));
    nodes_ = std::move(kept);
}

// Consumers, last uses and the invariant (cacheable) part of the graph.
void Graph::finish(const kern::Table& k) {
    for (Value& v : values_) {
        v.consumers.clear();
        v.lastUse = -1;
        v.producer = -1;
    }
    for (size_t i = 0; i < nodes_.size(); ++i) {
        Node& n = nodes_[i];
        bool inv = true;
        for (int v : n.in) {
            if (v < 0) continue;
            Value& val = values_[size_t(v)];
            val.consumers.push_back(int(i));
            val.lastUse = int(i);
            if (!val.isConst && !val.invariant) inv = false;
        }
        n.invariant = inv;
        // The column sums of a constant int8 weight, which the VNNI integer GEMM needs for the
        // zero point of its activations (the 16-bit one subtracts it while packing), once instead of
        // on every run.
        n.bSums.clear();
        if (k.ik == 4 && (n.op == Op::MatMulInteger || n.op == Op::MatMulIntegerScaled) && n.in.size() > 1 &&
            n.in[1] >= 0) {
            const Tensor& b = values_[size_t(n.in[1])].constant;
            if (values_[size_t(n.in[1])].isConst && b.type == DType::I8 && b.data && b.rank() == 2) {
                n.bSums.assign(size_t(b.dims[1]), 0);
                for (int64_t r = 0; r < b.dims[0]; ++r)
                    for (int64_t c = 0; c < b.dims[1]; ++c) n.bSums[size_t(c)] += b.as<int8_t>()[r * b.dims[1] + c];
            }
        }
        for (int v : n.out) {
            values_[size_t(v)].producer = int(i);
            values_[size_t(v)].invariant = inv;
        }
    }
    for (Value& v : values_) {
        v.keep = false;
        if (!v.invariant || v.isConst) continue;
        if (v.isOutput) v.keep = true;
        for (int c : v.consumers)
            if (!nodes_[size_t(c)].invariant) v.keep = true;
    }
}

void Session::setInput(int index, Tensor t) {
    if (slots_.size() != g_.values().size()) slots_.assign(g_.values().size(), Tensor());
    slots_[size_t(g_.inputs()[size_t(index)])] = std::move(t);
}

const Tensor& Session::output(int index) const {
    int v = g_.outputs()[size_t(index)];
    const Value& val = g_.values()[size_t(v)];
    return val.isConst ? val.constant : slots_[size_t(v)];
}

void Session::clearCache() {
    cached_ = false;
    for (size_t v = 0; v < slots_.size() && v < g_.values().size(); ++v)
        if (g_.values()[v].keep) slots_[v] = Tensor();
}

bool Session::run(const ExecContext& ctx, std::string* error, const std::atomic<bool>* cancel) {
    const std::vector<Value>& values = g_.values();
    const std::vector<Node>& nodes = g_.nodes();
    if (slots_.size() != values.size()) slots_.assign(values.size(), Tensor());
    for (int v : g_.inputs())
        if (!slots_[size_t(v)].valid() && !(cached_ && values[size_t(v)].invariant)) {
            if (error) *error = g_.label() + ": input " + values[size_t(v)].name + " not set";
            return false;
        }
    std::vector<const Tensor*> ins;
    std::vector<Tensor> outs;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const Node& n = nodes[i];
        if (cached_ && n.invariant) continue;
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            if (error) *error = "cancelled";
            return false;
        }
        ins.clear();
        for (int v : n.in) {
            if (v < 0) {
                ins.push_back(nullptr);
                continue;
            }
            const Value& val = values[size_t(v)];
            ins.push_back(val.isConst ? &val.constant : &slots_[size_t(v)]);
        }
        outs.assign(n.out.size(), Tensor());
        std::string err;
        std::chrono::steady_clock::time_point t0;
        bool profiling = g_profiling.load(std::memory_order_relaxed);
        if (profiling) t0 = std::chrono::steady_clock::now();
        if (!execNode(n, ins.data(), outs.data(), ctx, &err)) {
            if (error) *error = g_.label() + ": " + opName(n.op) + " " + n.name + ": " + err;
            return false;
        }
        if (profiling) {
            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            g_profile[int(n.op)] += sec;
            std::string key = g_.label() + " " + opName(n.op) + " " + n.name + " [";
            if (!outs.empty())
                for (size_t d = 0; d < outs[0].dims.size(); ++d) key += (d ? "," : "") + std::to_string(outs[0].dims[d]);
            g_profileNodes[key + "]"] += sec;
        }
        for (size_t o = 0; o < n.out.size(); ++o) slots_[size_t(n.out[o])] = std::move(outs[o]);
        if (keepAll_) continue;
        for (int v : n.in) {
            if (v < 0) continue;
            const Value& val = values[size_t(v)];
            if (val.lastUse == int(i) && !val.isConst && !val.isOutput && !val.keep) slots_[size_t(v)] = Tensor();
        }
    }
    cached_ = true;
    return true;
}

}  // namespace tts
