// row.h — column/value types and fixed row (de)serialization.
#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace mydb {

enum class ColType : uint8_t { INT = 0, REAL = 1, TEXT = 2 };

inline std::string col_type_name(ColType t) {
    switch (t) {
        case ColType::INT: return "INT";
        case ColType::REAL: return "REAL";
        case ColType::TEXT: return "TEXT";
    }
    return "?";
}

inline ColType col_type_from_name(const std::string& s) {
    if (s == "INT" || s == "INTEGER") return ColType::INT;
    if (s == "REAL" || s == "FLOAT" || s == "DOUBLE") return ColType::REAL;
    if (s == "TEXT" || s == "STRING" || s == "VARCHAR") return ColType::TEXT;
    throw std::runtime_error("unknown column type: " + s);
}

struct ColumnDef {
    std::string name;
    ColType type = ColType::TEXT;
    bool primary_key = false;
    bool not_null = false;
    bool unique = false;
    bool has_fk = false;
    std::string fk_table;
    std::string fk_column;
};

struct Value {
    ColType type = ColType::TEXT;
    bool is_null = false;
    int64_t i = 0;
    double d = 0.0;
    std::string s;

    static Value null_of(ColType t) { Value v; v.type = t; v.is_null = true; return v; }
    static Value make_int(int64_t x) { Value v; v.type = ColType::INT; v.i = x; return v; }
    static Value make_real(double x) { Value v; v.type = ColType::REAL; v.d = x; return v; }
    static Value make_text(std::string x) { Value v; v.type = ColType::TEXT; v.s = std::move(x); return v; }
};

inline bool values_equal(const Value& a, const Value& b) {
    if (a.is_null || b.is_null) return false; // SQL: NULL is never equal to anything, including NULL
    switch (a.type) {
        case ColType::INT: return a.i == b.i;
        case ColType::REAL: return a.d == b.d;
        case ColType::TEXT: return a.s == b.s;
    }
    return false;
}

// -1 / 0 / 1, comparing across INT/REAL numerically when mixed.
inline int compare_values(const Value& a, const Value& b) {
    bool a_num = a.type != ColType::TEXT, b_num = b.type != ColType::TEXT;
    if (a_num && b_num) {
        double x = a.type == ColType::INT ? (double)a.i : a.d;
        double y = b.type == ColType::INT ? (double)b.i : b.d;
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    return a.s < b.s ? -1 : (a.s > b.s ? 1 : 0);
}

// Fixed layout: [null-bitmap: ceil(N/8) bytes][non-null column values in
// column order]. INT -> 8 bytes, REAL -> 8 bytes, TEXT -> uint16 len + bytes.
// ponytail: values are asserted against MAX_ROW_BYTES by the b-tree layer,
// not here — this just packs bytes.
inline std::vector<uint8_t> encode_row(const std::vector<ColumnDef>& cols, const std::vector<Value>& values) {
    if (values.size() != cols.size()) throw std::runtime_error("encode_row: column count mismatch");
    std::vector<uint8_t> buf;
    size_t bitmap_bytes = (cols.size() + 7) / 8;
    buf.assign(bitmap_bytes, 0);
    for (size_t i = 0; i < cols.size(); i++) {
        if (values[i].is_null) buf[i / 8] |= (1 << (i % 8));
    }
    for (size_t i = 0; i < cols.size(); i++) {
        const Value& v = values[i];
        if (v.is_null) continue;
        switch (cols[i].type) {
            case ColType::INT: {
                size_t off = buf.size(); buf.resize(off + 8);
                std::memcpy(&buf[off], &v.i, 8);
                break;
            }
            case ColType::REAL: {
                size_t off = buf.size(); buf.resize(off + 8);
                std::memcpy(&buf[off], &v.d, 8);
                break;
            }
            case ColType::TEXT: {
                uint16_t len = static_cast<uint16_t>(v.s.size());
                size_t off = buf.size(); buf.resize(off + 2 + len);
                std::memcpy(&buf[off], &len, 2);
                if (len) std::memcpy(&buf[off + 2], v.s.data(), len);
                break;
            }
        }
    }
    return buf;
}

inline std::vector<Value> decode_row(const std::vector<ColumnDef>& cols, const std::vector<uint8_t>& buf) {
    std::vector<Value> out;
    out.reserve(cols.size());
    size_t bitmap_bytes = (cols.size() + 7) / 8;
    size_t off = bitmap_bytes;
    for (size_t i = 0; i < cols.size(); i++) {
        bool is_null = (buf[i / 8] & (1 << (i % 8))) != 0;
        Value v;
        v.type = cols[i].type;
        if (is_null) { v.is_null = true; out.push_back(v); continue; }
        switch (cols[i].type) {
            case ColType::INT:
                std::memcpy(&v.i, &buf[off], 8); off += 8; break;
            case ColType::REAL:
                std::memcpy(&v.d, &buf[off], 8); off += 8; break;
            case ColType::TEXT: {
                uint16_t len; std::memcpy(&len, &buf[off], 2); off += 2;
                v.s.assign(buf.begin() + off, buf.begin() + off + len);
                off += len;
                break;
            }
        }
        out.push_back(v);
    }
    return out;
}

} // namespace mydb
