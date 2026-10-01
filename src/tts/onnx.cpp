#include "onnx.h"
#include <cstring>

namespace tts {
namespace onnx {
namespace {

// Protobuf wire types.
enum Wire : int { kVarint = 0, kFixed64 = 1, kBytes = 2, kFixed32 = 5 };

// Cursor over one message. Every read checks the bounds; a malformed file sets 'bad' and the
// parse stops at the next check.
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool bad = false;

    Reader(const uint8_t* b, size_t n) : p(b), end(b + n) {}
    bool more() const { return !bad && p < end; }

    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p >= end) {
                bad = true;
                return 0;
            }
            uint8_t b = *p++;
            v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        bad = true;
        return 0;
    }
    bool tag(int& field, int& wire) {
        uint64_t t = varint();
        field = int(t >> 3);
        wire = int(t & 7);
        return !bad && field > 0;
    }
    uint32_t fixed32() {
        if (end - p < 4) {
            bad = true;
            return 0;
        }
        uint32_t v;
        std::memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    uint64_t fixed64() {
        if (end - p < 8) {
            bad = true;
            return 0;
        }
        uint64_t v;
        std::memcpy(&v, p, 8);
        p += 8;
        return v;
    }
    // Length-delimited payload: returns a sub-reader and advances past it.
    Reader bytes() {
        uint64_t n = varint();
        if (bad || n > uint64_t(end - p)) {
            bad = true;
            return Reader(p, 0);
        }
        Reader r(p, size_t(n));
        p += n;
        return r;
    }
    std::string string() {
        Reader r = bytes();
        return std::string(reinterpret_cast<const char*>(r.p), size_t(r.end - r.p));
    }
    void skip(int wire) {
        switch (wire) {
        case kVarint: varint(); break;
        case kFixed64: fixed64(); break;
        case kBytes: bytes(); break;
        case kFixed32: fixed32(); break;
        default: bad = true; break;
        }
    }
};

// Repeated scalar fields may be packed (one length-delimited run) or not (one tag per value).
void readInts(Reader& r, int wire, std::vector<int64_t>& out) {
    if (wire == kBytes) {
        Reader s = r.bytes();
        while (s.more()) out.push_back(int64_t(s.varint()));
        if (s.bad) r.bad = true;
    } else if (wire == kVarint) {
        out.push_back(int64_t(r.varint()));
    } else {
        r.bad = true;
    }
}

void readFloats(Reader& r, int wire, std::vector<float>& out) {
    auto one = [&out](uint32_t bits) {
        float f;
        std::memcpy(&f, &bits, 4);
        out.push_back(f);
    };
    if (wire == kBytes) {
        Reader s = r.bytes();
        while (s.more()) one(s.fixed32());
        if (s.bad) r.bad = true;
    } else if (wire == kFixed32) {
        one(r.fixed32());
    } else {
        r.bad = true;
    }
}

size_t elementSize(int dataType) {
    switch (dataType) {
    case kFloat: case kInt32: return 4;
    case kUint8: case kInt8: case kBool: return 1;
    case kInt64: return 8;
    default: return 0;
    }
}

void parseTensor(Reader r, TensorData& t) {
    std::vector<float> floats;
    std::vector<int64_t> ints32, ints64;
    int field, wire;
    while (r.more() && r.tag(field, wire)) {
        switch (field) {
        case 1: readInts(r, wire, t.dims); break;
        case 2: t.dataType = int(r.varint()); break;
        case 4: readFloats(r, wire, floats); break;
        case 5: readInts(r, wire, ints32); break;
        case 7: readInts(r, wire, ints64); break;
        case 8: t.name = r.string(); break;
        case 9: {
            Reader s = r.bytes();
            t.raw = s.p;
            t.rawSize = size_t(s.end - s.p);
            break;
        }
        case 13: case 14: r.bad = true; break;   // external data: not supported
        default: r.skip(wire); break;
        }
    }
    if (t.raw) return;
    // Typed storage: float_data for FLOAT; int32_data for INT32 and the 8-bit / bool types (one
    // value per element); int64_data for INT64.
    size_t es = elementSize(t.dataType);
    if (t.dataType == kFloat) {
        t.decoded.resize(floats.size() * 4);
        if (!floats.empty()) std::memcpy(t.decoded.data(), floats.data(), t.decoded.size());
    } else if (t.dataType == kInt64) {
        t.decoded.resize(ints64.size() * 8);
        if (!ints64.empty()) std::memcpy(t.decoded.data(), ints64.data(), t.decoded.size());
    } else if (es == 4 || es == 1) {
        t.decoded.resize(ints32.size() * es);
        for (size_t i = 0; i < ints32.size(); ++i) {
            if (es == 4) {
                int32_t v = int32_t(ints32[i]);
                std::memcpy(&t.decoded[i * 4], &v, 4);
            } else {
                t.decoded[i] = uint8_t(ints32[i] & 0xff);
            }
        }
    }
}

void parseAttribute(Reader r, Attribute& a) {
    int field, wire;
    while (r.more() && r.tag(field, wire)) {
        switch (field) {
        case 1: a.name = r.string(); break;
        case 2: {
            uint32_t bits = r.fixed32();
            std::memcpy(&a.f, &bits, 4);
            break;
        }
        case 3: a.i = int64_t(r.varint()); break;
        case 4: a.s = r.string(); break;
        case 5: parseTensor(r.bytes(), a.t); break;
        case 7: readFloats(r, wire, a.floats); break;
        case 8: readInts(r, wire, a.ints); break;
        case 20: a.type = int(r.varint()); break;
        default: r.skip(wire); break;
        }
    }
}

void parseNode(Reader r, NodeProto& n) {
    int field, wire;
    while (r.more() && r.tag(field, wire)) {
        switch (field) {
        case 1: n.inputs.push_back(r.string()); break;
        case 2: n.outputs.push_back(r.string()); break;
        case 3: n.name = r.string(); break;
        case 4: n.opType = r.string(); break;
        case 5: {
            n.attributes.emplace_back();
            parseAttribute(r.bytes(), n.attributes.back());
            break;
        }
        case 7: n.domain = r.string(); break;
        default: r.skip(wire); break;
        }
    }
}

void parseValueInfo(Reader r, ValueInfo& v) {
    int field, wire;
    while (r.more() && r.tag(field, wire)) {
        if (field == 1) {
            v.name = r.string();
        } else if (field == 2) {                      // TypeProto
            Reader tp = r.bytes();
            int f2, w2;
            while (tp.more() && tp.tag(f2, w2)) {
                if (f2 != 1) {                         // tensor_type
                    tp.skip(w2);
                    continue;
                }
                Reader tt = tp.bytes();
                int f3, w3;
                while (tt.more() && tt.tag(f3, w3)) {
                    if (f3 == 1) {
                        v.elemType = int(tt.varint());
                    } else if (f3 == 2) {              // TensorShapeProto
                        Reader sh = tt.bytes();
                        int f4, w4;
                        while (sh.more() && sh.tag(f4, w4)) {
                            if (f4 != 1) {
                                sh.skip(w4);
                                continue;
                            }
                            Reader dim = sh.bytes();
                            int64_t value = -1;
                            int f5, w5;
                            while (dim.more() && dim.tag(f5, w5)) {
                                if (f5 == 1) value = int64_t(dim.varint());
                                else dim.skip(w5);
                            }
                            v.dims.push_back(value);
                        }
                    } else {
                        tt.skip(w3);
                    }
                }
            }
        } else {
            r.skip(wire);
        }
    }
}

bool parseGraph(Reader r, Model& m) {
    int field, wire;
    while (r.more() && r.tag(field, wire)) {
        switch (field) {
        case 1: m.nodes.emplace_back(); parseNode(r.bytes(), m.nodes.back()); break;
        case 5: m.initializers.emplace_back(); parseTensor(r.bytes(), m.initializers.back()); break;
        case 11: m.inputs.emplace_back(); parseValueInfo(r.bytes(), m.inputs.back()); break;
        case 12: m.outputs.emplace_back(); parseValueInfo(r.bytes(), m.outputs.back()); break;
        default: r.skip(wire); break;
        }
    }
    return !r.bad;
}

}  // namespace

int64_t TensorData::elementCount() const {
    int64_t n = 1;
    for (int64_t d : dims) n *= d;
    return n;
}

const Attribute* NodeProto::attribute(const char* attrName) const {
    for (const Attribute& a : attributes)
        if (a.name == attrName) return &a;
    return nullptr;
}

bool parse(const uint8_t* data, size_t size, Model& out, std::string* error) {
    out = Model();
    Reader r(data, size);
    bool haveGraph = false;
    int field, wire;
    while (r.more() && r.tag(field, wire)) {
        switch (field) {
        case 1: out.irVersion = int64_t(r.varint()); break;
        case 2: out.producer = r.string(); break;
        case 7: {
            Reader g = r.bytes();
            if (!parseGraph(g, out)) r.bad = true;
            haveGraph = true;
            break;
        }
        case 8: {                                     // OperatorSetIdProto
            Reader o = r.bytes();
            std::string domain;
            int64_t version = 0;
            int f2, w2;
            while (o.more() && o.tag(f2, w2)) {
                if (f2 == 1) domain = o.string();
                else if (f2 == 2) version = int64_t(o.varint());
                else o.skip(w2);
            }
            if (domain.empty() || domain == "ai.onnx") out.opset = version;
            break;
        }
        default: r.skip(wire); break;
        }
    }
    if (r.bad || !haveGraph) {
        if (error) *error = r.bad ? "malformed ONNX protobuf" : "no graph in the ONNX file";
        return false;
    }
    for (const TensorData& t : out.initializers) {
        size_t es = elementSize(t.dataType);
        if (es == 0 || t.byteSize() != size_t(t.elementCount()) * es) {
            if (error) *error = "initializer " + t.name + ": unsupported type or size mismatch";
            return false;
        }
    }
    return true;
}

}  // namespace onnx
}  // namespace tts
