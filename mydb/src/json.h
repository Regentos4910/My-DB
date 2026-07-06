// json.h — minimal JSON value/parser/serializer.
//
// ponytail: hand-rolled because the sandbox this was built in has neither
// internet access nor package-manager root, so nlohmann/json (the obvious
// off-the-shelf pick) couldn't be fetched or installed. If your machine
// has it available, swap this out; the surface used elsewhere (object(),
// set(), array(), push(), dump(), parse_json()) is intentionally small.
#pragma once
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mydb {

class JsonValue {
public:
    enum class Type { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

    Type type = Type::NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    static JsonValue null_() { return JsonValue(); }
    static JsonValue boolean(bool v) { JsonValue j; j.type = Type::BOOL; j.b = v; return j; }
    static JsonValue number(double v) { JsonValue j; j.type = Type::NUMBER; j.num = v; return j; }
    static JsonValue string(std::string v) { JsonValue j; j.type = Type::STRING; j.str = std::move(v); return j; }
    static JsonValue array() { JsonValue j; j.type = Type::ARRAY; return j; }
    static JsonValue object() { JsonValue j; j.type = Type::OBJECT; return j; }

    void set(const std::string& key, JsonValue v) {
        for (auto& kv : obj) if (kv.first == key) { kv.second = std::move(v); return; }
        obj.emplace_back(key, std::move(v));
    }
    void push(JsonValue v) { arr.push_back(std::move(v)); }

    const JsonValue* find(const std::string& key) const {
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }

    bool is_null() const { return type == Type::NUL; }
    std::string as_string(const std::string& def = "") const { return type == Type::STRING ? str : def; }
    double as_number(double def = 0) const { return type == Type::NUMBER ? num : def; }
    bool as_bool(bool def = false) const { return type == Type::BOOL ? b : def; }

    std::string dump() const {
        std::ostringstream os;
        dump_to(os);
        return os.str();
    }

private:
    static void escape_into(std::ostringstream& os, const std::string& s) {
        os << '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': os << "\\\""; break;
                case '\\': os << "\\\\"; break;
                case '\n': os << "\\n"; break;
                case '\r': os << "\\r"; break;
                case '\t': os << "\\t"; break;
                default:
                    if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); os << buf; }
                    else os << static_cast<char>(c);
            }
        }
        os << '"';
    }

    void dump_to(std::ostringstream& os) const {
        switch (type) {
            case Type::NUL: os << "null"; break;
            case Type::BOOL: os << (b ? "true" : "false"); break;
            case Type::NUMBER: {
                if (num == static_cast<long long>(num)) os << static_cast<long long>(num);
                else os << num;
                break;
            }
            case Type::STRING: escape_into(os, str); break;
            case Type::ARRAY: {
                os << '[';
                for (size_t i = 0; i < arr.size(); i++) { if (i) os << ','; arr[i].dump_to(os); }
                os << ']';
                break;
            }
            case Type::OBJECT: {
                os << '{';
                for (size_t i = 0; i < obj.size(); i++) {
                    if (i) os << ',';
                    escape_into(os, obj[i].first);
                    os << ':';
                    obj[i].second.dump_to(os);
                }
                os << '}';
                break;
            }
        }
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    JsonValue parse() {
        skip_ws();
        if (i_ >= s_.size()) return JsonValue::null_();
        return parse_value();
    }

private:
    void skip_ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) i_++; }
    char peek() { return i_ < s_.size() ? s_[i_] : '\0'; }

    JsonValue parse_value() {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return JsonValue::string(parse_string());
        if (c == 't') { expect_lit("true"); return JsonValue::boolean(true); }
        if (c == 'f') { expect_lit("false"); return JsonValue::boolean(false); }
        if (c == 'n') { expect_lit("null"); return JsonValue::null_(); }
        return parse_number();
    }

    void expect_lit(const std::string& lit) {
        if (s_.compare(i_, lit.size(), lit) != 0) throw std::runtime_error("json: expected literal " + lit);
        i_ += lit.size();
    }

    std::string parse_string() {
        if (peek() != '"') throw std::runtime_error("json: expected string");
        i_++;
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size()) {
                char n = s_[i_ + 1];
                switch (n) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'u': {
                        if (i_ + 5 < s_.size()) {
                            int code = std::stoi(s_.substr(i_ + 2, 4), nullptr, 16);
                            if (code < 0x80) out += static_cast<char>(code);
                            else out += '?'; // ponytail: no full utf-8 encode of \uXXXX, ascii-safe for a college demo
                            i_ += 4;
                        }
                        break;
                    }
                    default: out += n;
                }
                i_ += 2;
            } else { out += c; i_++; }
        }
        if (i_ >= s_.size()) throw std::runtime_error("json: unterminated string");
        i_++; // closing quote
        return out;
    }

    JsonValue parse_number() {
        size_t start = i_;
        if (peek() == '-') i_++;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) i_++;
        if (i_ == start) throw std::runtime_error("json: expected number");
        return JsonValue::number(std::stod(s_.substr(start, i_ - start)));
    }

    JsonValue parse_array() {
        i_++; // [
        JsonValue v = JsonValue::array();
        skip_ws();
        if (peek() == ']') { i_++; return v; }
        while (true) {
            v.push(parse_value());
            skip_ws();
            if (peek() == ',') { i_++; continue; }
            if (peek() == ']') { i_++; break; }
            throw std::runtime_error("json: expected , or ] in array");
        }
        return v;
    }

    JsonValue parse_object() {
        i_++; // {
        JsonValue v = JsonValue::object();
        skip_ws();
        if (peek() == '}') { i_++; return v; }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            if (peek() != ':') throw std::runtime_error("json: expected :");
            i_++;
            v.set(key, parse_value());
            skip_ws();
            if (peek() == ',') { i_++; continue; }
            if (peek() == '}') { i_++; break; }
            throw std::runtime_error("json: expected , or } in object");
        }
        return v;
    }

    const std::string& s_;
    size_t i_ = 0;
};

inline JsonValue parse_json(const std::string& s) { return JsonParser(s).parse(); }

} // namespace mydb
