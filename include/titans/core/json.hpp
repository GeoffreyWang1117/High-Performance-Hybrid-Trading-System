/**
 * @file json.hpp
 * @brief Lightweight JSON Parser and Builder
 *
 * Single-header JSON library for LLM API integration.
 * Supports parsing, building, and serialization.
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <optional>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <cmath>
#include <cstring>
#include <cctype>

namespace titans {
namespace json {

// Forward declaration
class Value;

// JSON types
using Null = std::nullptr_t;
using Bool = bool;
using Number = double;
using String = std::string;
using Array = std::vector<Value>;
using Object = std::unordered_map<std::string, Value>;

// ============================================================================
// JSON Value
// ============================================================================

class Value {
public:
    using Variant = std::variant<Null, Bool, Number, String, Array, Object>;

    // Constructors
    Value() : data_(nullptr) {}
    Value(std::nullptr_t) : data_(nullptr) {}
    Value(bool b) : data_(b) {}
    Value(int n) : data_(static_cast<double>(n)) {}
    Value(int64_t n) : data_(static_cast<double>(n)) {}
    Value(double n) : data_(n) {}
    Value(const char* s) : data_(std::string(s)) {}
    Value(const std::string& s) : data_(s) {}
    Value(std::string&& s) : data_(std::move(s)) {}
    Value(const Array& a) : data_(a) {}
    Value(Array&& a) : data_(std::move(a)) {}
    Value(const Object& o) : data_(o) {}
    Value(Object&& o) : data_(std::move(o)) {}
    Value(std::initializer_list<std::pair<const std::string, Value>> init)
        : data_(Object(init.begin(), init.end())) {}

    // Type checks
    bool is_null() const { return std::holds_alternative<Null>(data_); }
    bool is_bool() const { return std::holds_alternative<Bool>(data_); }
    bool is_number() const { return std::holds_alternative<Number>(data_); }
    bool is_string() const { return std::holds_alternative<String>(data_); }
    bool is_array() const { return std::holds_alternative<Array>(data_); }
    bool is_object() const { return std::holds_alternative<Object>(data_); }

    // Getters with defaults
    bool as_bool(bool default_val = false) const {
        return is_bool() ? std::get<Bool>(data_) : default_val;
    }

    double as_number(double default_val = 0.0) const {
        return is_number() ? std::get<Number>(data_) : default_val;
    }

    int as_int(int default_val = 0) const {
        return is_number() ? static_cast<int>(std::get<Number>(data_)) : default_val;
    }

    int64_t as_int64(int64_t default_val = 0) const {
        return is_number() ? static_cast<int64_t>(std::get<Number>(data_)) : default_val;
    }

    const std::string& as_string(const std::string& default_val = "") const {
        static const std::string empty;
        return is_string() ? std::get<String>(data_) : (default_val.empty() ? empty : default_val);
    }

    const Array& as_array() const {
        static const Array empty;
        return is_array() ? std::get<Array>(data_) : empty;
    }

    Array& as_array() {
        if (!is_array()) data_ = Array{};
        return std::get<Array>(data_);
    }

    const Object& as_object() const {
        static const Object empty;
        return is_object() ? std::get<Object>(data_) : empty;
    }

    Object& as_object() {
        if (!is_object()) data_ = Object{};
        return std::get<Object>(data_);
    }

    // Array access
    const Value& operator[](size_t index) const {
        static const Value null_val;
        if (!is_array()) return null_val;
        const auto& arr = std::get<Array>(data_);
        return index < arr.size() ? arr[index] : null_val;
    }

    Value& operator[](size_t index) {
        if (!is_array()) data_ = Array{};
        auto& arr = std::get<Array>(data_);
        if (index >= arr.size()) arr.resize(index + 1);
        return arr[index];
    }

    // Object access
    const Value& operator[](const std::string& key) const {
        static const Value null_val;
        if (!is_object()) return null_val;
        const auto& obj = std::get<Object>(data_);
        auto it = obj.find(key);
        return it != obj.end() ? it->second : null_val;
    }

    Value& operator[](const std::string& key) {
        if (!is_object()) data_ = Object{};
        return std::get<Object>(data_)[key];
    }

    bool contains(const std::string& key) const {
        if (!is_object()) return false;
        return std::get<Object>(data_).count(key) > 0;
    }

    size_t size() const {
        if (is_array()) return std::get<Array>(data_).size();
        if (is_object()) return std::get<Object>(data_).size();
        return 0;
    }

    // Array operations
    void push_back(const Value& v) {
        if (!is_array()) data_ = Array{};
        std::get<Array>(data_).push_back(v);
    }

    void push_back(Value&& v) {
        if (!is_array()) data_ = Array{};
        std::get<Array>(data_).push_back(std::move(v));
    }

    // Get underlying variant
    const Variant& data() const { return data_; }
    Variant& data() { return data_; }

private:
    Variant data_;
};

// ============================================================================
// JSON Parser
// ============================================================================

class Parser {
public:
    static Value parse(const std::string& json) {
        Parser p(json);
        return p.parse_value();
    }

    static std::optional<Value> try_parse(const std::string& json) {
        try {
            return parse(json);
        } catch (...) {
            return std::nullopt;
        }
    }

private:
    explicit Parser(const std::string& json) : json_(json), pos_(0) {}

    Value parse_value() {
        skip_whitespace();
        if (pos_ >= json_.size()) throw std::runtime_error("Unexpected end of input");

        char c = json_[pos_];
        if (c == 'n') return parse_null();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == '"') return parse_string();
        if (c == '[') return parse_array();
        if (c == '{') return parse_object();
        if (c == '-' || std::isdigit(c)) return parse_number();

        throw std::runtime_error("Unexpected character: " + std::string(1, c));
    }

    Value parse_null() {
        expect("null");
        return Value(nullptr);
    }

    Value parse_bool() {
        if (json_.substr(pos_, 4) == "true") {
            pos_ += 4;
            return Value(true);
        }
        if (json_.substr(pos_, 5) == "false") {
            pos_ += 5;
            return Value(false);
        }
        throw std::runtime_error("Expected 'true' or 'false'");
    }

    Value parse_number() {
        size_t start = pos_;
        if (json_[pos_] == '-') ++pos_;

        while (pos_ < json_.size() && std::isdigit(json_[pos_])) ++pos_;

        if (pos_ < json_.size() && json_[pos_] == '.') {
            ++pos_;
            while (pos_ < json_.size() && std::isdigit(json_[pos_])) ++pos_;
        }

        if (pos_ < json_.size() && (json_[pos_] == 'e' || json_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < json_.size() && (json_[pos_] == '+' || json_[pos_] == '-')) ++pos_;
            while (pos_ < json_.size() && std::isdigit(json_[pos_])) ++pos_;
        }

        return Value(std::stod(json_.substr(start, pos_ - start)));
    }

    Value parse_string() {
        expect('"');
        std::string result;

        while (pos_ < json_.size() && json_[pos_] != '"') {
            if (json_[pos_] == '\\') {
                ++pos_;
                if (pos_ >= json_.size()) throw std::runtime_error("Unexpected end in string escape");
                switch (json_[pos_]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case 'u': {
                        if (pos_ + 4 >= json_.size())
                            throw std::runtime_error("Invalid unicode escape");
                        std::string hex = json_.substr(pos_ + 1, 4);
                        int codepoint = std::stoi(hex, nullptr, 16);
                        if (codepoint < 0x80) {
                            result += static_cast<char>(codepoint);
                        } else if (codepoint < 0x800) {
                            result += static_cast<char>(0xC0 | (codepoint >> 6));
                            result += static_cast<char>(0x80 | (codepoint & 0x3F));
                        } else {
                            result += static_cast<char>(0xE0 | (codepoint >> 12));
                            result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (codepoint & 0x3F));
                        }
                        pos_ += 4;
                        break;
                    }
                    default:
                        throw std::runtime_error("Invalid escape sequence");
                }
            } else {
                result += json_[pos_];
            }
            ++pos_;
        }

        expect('"');
        return Value(std::move(result));
    }

    Value parse_array() {
        expect('[');
        Array arr;

        skip_whitespace();
        if (pos_ < json_.size() && json_[pos_] == ']') {
            ++pos_;
            return Value(std::move(arr));
        }

        while (true) {
            arr.push_back(parse_value());
            skip_whitespace();

            if (pos_ >= json_.size()) throw std::runtime_error("Unexpected end in array");
            if (json_[pos_] == ']') {
                ++pos_;
                break;
            }
            expect(',');
        }

        return Value(std::move(arr));
    }

    Value parse_object() {
        expect('{');
        Object obj;

        skip_whitespace();
        if (pos_ < json_.size() && json_[pos_] == '}') {
            ++pos_;
            return Value(std::move(obj));
        }

        while (true) {
            skip_whitespace();
            if (pos_ >= json_.size() || json_[pos_] != '"')
                throw std::runtime_error("Expected string key in object");

            std::string key = parse_string().as_string();
            skip_whitespace();
            expect(':');
            obj[key] = parse_value();
            skip_whitespace();

            if (pos_ >= json_.size()) throw std::runtime_error("Unexpected end in object");
            if (json_[pos_] == '}') {
                ++pos_;
                break;
            }
            expect(',');
        }

        return Value(std::move(obj));
    }

    void skip_whitespace() {
        while (pos_ < json_.size() && std::isspace(json_[pos_])) ++pos_;
    }

    void expect(char c) {
        skip_whitespace();
        if (pos_ >= json_.size() || json_[pos_] != c) {
            throw std::runtime_error("Expected '" + std::string(1, c) + "'");
        }
        ++pos_;
    }

    void expect(const char* s) {
        size_t len = std::strlen(s);
        if (json_.substr(pos_, len) != s) {
            throw std::runtime_error("Expected '" + std::string(s) + "'");
        }
        pos_ += len;
    }

    const std::string& json_;
    size_t pos_;
};

// ============================================================================
// JSON Serializer
// ============================================================================

class Serializer {
public:
    static std::string stringify(const Value& v, bool pretty = false, int indent = 2) {
        Serializer s(pretty, indent);
        s.serialize(v, 0);
        return s.result_.str();
    }

private:
    Serializer(bool pretty, int indent) : pretty_(pretty), indent_(indent) {}

    void serialize(const Value& v, int depth) {
        if (v.is_null()) {
            result_ << "null";
        } else if (v.is_bool()) {
            result_ << (v.as_bool() ? "true" : "false");
        } else if (v.is_number()) {
            double n = v.as_number();
            if (std::floor(n) == n && n < 1e15 && n > -1e15) {
                result_ << static_cast<int64_t>(n);
            } else {
                result_ << std::setprecision(15) << n;
            }
        } else if (v.is_string()) {
            serialize_string(v.as_string());
        } else if (v.is_array()) {
            serialize_array(v.as_array(), depth);
        } else if (v.is_object()) {
            serialize_object(v.as_object(), depth);
        }
    }

    void serialize_string(const std::string& s) {
        result_ << '"';
        for (char c : s) {
            switch (c) {
                case '"': result_ << "\\\""; break;
                case '\\': result_ << "\\\\"; break;
                case '\b': result_ << "\\b"; break;
                case '\f': result_ << "\\f"; break;
                case '\n': result_ << "\\n"; break;
                case '\r': result_ << "\\r"; break;
                case '\t': result_ << "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        result_ << "\\u" << std::hex << std::setw(4)
                                << std::setfill('0') << static_cast<int>(c);
                    } else {
                        result_ << c;
                    }
            }
        }
        result_ << '"';
    }

    void serialize_array(const Array& arr, int depth) {
        if (arr.empty()) {
            result_ << "[]";
            return;
        }

        result_ << '[';
        if (pretty_) result_ << '\n';

        for (size_t i = 0; i < arr.size(); ++i) {
            if (pretty_) write_indent(depth + 1);
            serialize(arr[i], depth + 1);
            if (i < arr.size() - 1) result_ << ',';
            if (pretty_) result_ << '\n';
        }

        if (pretty_) write_indent(depth);
        result_ << ']';
    }

    void serialize_object(const Object& obj, int depth) {
        if (obj.empty()) {
            result_ << "{}";
            return;
        }

        result_ << '{';
        if (pretty_) result_ << '\n';

        size_t i = 0;
        for (const auto& [key, val] : obj) {
            if (pretty_) write_indent(depth + 1);
            serialize_string(key);
            result_ << ':';
            if (pretty_) result_ << ' ';
            serialize(val, depth + 1);
            if (i < obj.size() - 1) result_ << ',';
            if (pretty_) result_ << '\n';
            ++i;
        }

        if (pretty_) write_indent(depth);
        result_ << '}';
    }

    void write_indent(int depth) {
        for (int i = 0; i < depth * indent_; ++i) result_ << ' ';
    }

    std::ostringstream result_;
    bool pretty_;
    int indent_;
};

// ============================================================================
// Convenience Functions
// ============================================================================

inline Value parse(const std::string& json) {
    return Parser::parse(json);
}

inline std::optional<Value> try_parse(const std::string& json) {
    return Parser::try_parse(json);
}

inline std::string stringify(const Value& v, bool pretty = false) {
    return Serializer::stringify(v, pretty);
}

// Object builder helper
inline Value object(std::initializer_list<std::pair<const std::string, Value>> init) {
    return Value(Object(init.begin(), init.end()));
}

// Array builder helper
inline Value array(std::initializer_list<Value> init) {
    return Value(Array(init.begin(), init.end()));
}

}  // namespace json
}  // namespace titans
