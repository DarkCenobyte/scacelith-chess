// Small JSON reader/writer for the HTTPS API bodies of the dedicated server (and the credential
// file). RFC 8259: objects, arrays, strings (escapes, \u with surrogate pairs), numbers, true,
// false, null. Strict: invalid UTF-8, raw control characters, lone surrogates, trailing commas,
// leading zeros and trailing garbage are errors. Limits keep a hostile server from making the
// client allocate without bound (input size, nesting depth, element count).
//
// Numbers are doubles (integers up to 2^53 are exact, which covers every id the server sends).
// Objects keep their member order; lookups are linear (API bodies are small).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace net {
namespace json {

class Value {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), b_(b) {}
    Value(int v) : type_(Type::Number), n_(v) {}
    Value(unsigned v) : type_(Type::Number), n_(v) {}
    Value(int64_t v) : type_(Type::Number), n_(double(v)) {}
    Value(uint64_t v) : type_(Type::Number), n_(double(v)) {}
    Value(double v) : type_(Type::Number), n_(v) {}
    Value(const char* s) : type_(Type::String), s_(s) {}
    Value(std::string s) : type_(Type::String), s_(std::move(s)) {}
    static Value array() { Value v; v.type_ = Type::Array; return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // Typed reads with a default when the value has another type.
    bool asBool(bool def = false) const { return type_ == Type::Bool ? b_ : def; }
    double asNumber(double def = 0) const { return type_ == Type::Number ? n_ : def; }
    int64_t asInt(int64_t def = 0) const;   // numbers only, truncated towards zero, clamped to +-2^63
    const std::string& asString() const;    // "" when not a string
    std::string asString(const std::string& def) const { return type_ == Type::String ? s_ : def; }

    // Arrays and objects. operator[] returns a shared null value when absent / out of range.
    size_t size() const { return type_ == Type::Array ? items_.size() : type_ == Type::Object ? members_.size() : 0; }
    const Value& operator[](size_t i) const;
    const Value& operator[](int i) const { return (*this)[i < 0 ? size_t(-1) : size_t(i)]; }
    const Value& operator[](const std::string& key) const;
    const Value& operator[](const char* key) const { return (*this)[std::string(key)]; }
    bool has(const std::string& key) const;
    const std::vector<Value>& items() const { return items_; }
    const std::vector<std::pair<std::string, Value>>& members() const { return members_; }

    // Building (turns a null value into an array / object).
    Value& push(Value v);
    Value& set(const std::string& key, Value v);   // replaces an existing member

    // Compact serialisation (UTF-8, "/" not escaped, non-finite numbers written as null).
    std::string dump() const;

private:
    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Value> items_;
    std::vector<std::pair<std::string, Value>> members_;
    void dumpTo(std::string& out) const;
    friend class Parser;
};

struct Limits {
    size_t maxBytes = 1 << 20;     // input size
    int maxDepth = 32;             // nesting of arrays/objects
    size_t maxElements = 100000;   // values in the whole document
};

// Parses a whole document (surrounding whitespace allowed). On failure returns false and, when
// error is given, a short description with the byte offset.
bool parse(const std::string& text, Value& out, std::string* error = nullptr, const Limits& limits = Limits());

// Appends s as a JSON string literal (quotes included).
void quote(const std::string& s, std::string& out);

}  // namespace json
}  // namespace net
