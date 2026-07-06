// catalog.h — schema metadata (table definitions), itself stored in a
// b-tree so it durably persists in the same file as the data.
//
// ponytail: the whole catalog is loaded into memory on open and rewritten
// whole-row on every change. Fine at "a handful of tables" college-project
// scale; a real system would update just the changed row and keep an
// index instead of a full in-memory copy.
#pragma once
#include "btree.h"
#include "row.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace mydb {

struct TableDef {
    uint32_t table_id = 0;
    std::string name;
    uint32_t root_page = 0;
    uint64_t next_rowid = 1;
    std::vector<ColumnDef> columns;
};

namespace detail {

inline void write_string(std::vector<uint8_t>& buf, const std::string& s) {
    uint16_t len = static_cast<uint16_t>(s.size());
    size_t off = buf.size(); buf.resize(off + 2 + len);
    std::memcpy(&buf[off], &len, 2);
    if (len) std::memcpy(&buf[off + 2], s.data(), len);
}

inline std::string read_string(const std::vector<uint8_t>& buf, size_t& off) {
    uint16_t len; std::memcpy(&len, &buf[off], 2); off += 2;
    std::string s(buf.begin() + off, buf.begin() + off + len);
    off += len;
    return s;
}

} // namespace detail

inline std::vector<uint8_t> encode_table_def(const TableDef& def) {
    std::vector<uint8_t> buf;
    buf.resize(4 + 4 + 8);
    std::memcpy(&buf[0], &def.table_id, 4);
    std::memcpy(&buf[4], &def.root_page, 4);
    std::memcpy(&buf[8], &def.next_rowid, 8);
    detail::write_string(buf, def.name);
    uint16_t num_cols = static_cast<uint16_t>(def.columns.size());
    size_t off = buf.size(); buf.resize(off + 2);
    std::memcpy(&buf[off], &num_cols, 2);
    for (auto& c : def.columns) {
        detail::write_string(buf, c.name);
        buf.push_back(static_cast<uint8_t>(c.type));
        uint8_t flags = (c.primary_key ? 1 : 0) | (c.not_null ? 2 : 0) | (c.unique ? 4 : 0) | (c.has_fk ? 8 : 0);
        buf.push_back(flags);
        detail::write_string(buf, c.fk_table);
        detail::write_string(buf, c.fk_column);
    }
    return buf;
}

inline TableDef decode_table_def(const std::vector<uint8_t>& buf) {
    TableDef def;
    std::memcpy(&def.table_id, &buf[0], 4);
    std::memcpy(&def.root_page, &buf[4], 4);
    std::memcpy(&def.next_rowid, &buf[8], 8);
    size_t off = 16;
    def.name = detail::read_string(buf, off);
    uint16_t num_cols; std::memcpy(&num_cols, &buf[off], 2); off += 2;
    def.columns.resize(num_cols);
    for (auto& c : def.columns) {
        c.name = detail::read_string(buf, off);
        c.type = static_cast<ColType>(buf[off]); off += 1;
        uint8_t flags = buf[off]; off += 1;
        c.primary_key = flags & 1; c.not_null = flags & 2; c.unique = flags & 4; c.has_fk = flags & 8;
        c.fk_table = detail::read_string(buf, off);
        c.fk_column = detail::read_string(buf, off);
    }
    return def;
}

class Catalog {
public:
    explicit Catalog(Pager& pager) : pager_(pager), tree_(pager, pager.catalog_root()) {
        tree_.scan([&](uint64_t, const std::vector<uint8_t>& blob) {
            TableDef def = decode_table_def(blob);
            by_name_[def.name] = def;
            return true;
        });
    }

    bool has_table(const std::string& name) const { return by_name_.count(name) > 0; }

    TableDef& get(const std::string& name) {
        auto it = by_name_.find(name);
        if (it == by_name_.end()) throw std::runtime_error("no such table: " + name);
        return it->second;
    }

    std::vector<std::string> table_names() const {
        std::vector<std::string> names;
        for (auto& kv : by_name_) names.push_back(kv.first);
        std::sort(names.begin(), names.end());
        return names;
    }

    TableDef create_table(const std::string& name, std::vector<ColumnDef> columns) {
        if (has_table(name)) throw std::runtime_error("table already exists: " + name);
        TableDef def;
        def.table_id = pager_.next_table_id();
        def.name = name;
        def.root_page = BTree::create_empty(pager_);
        def.next_rowid = 1;
        def.columns = std::move(columns);
        tree_.insert(def.table_id, encode_table_def(def));
        sync_root();
        by_name_[name] = def;
        return def;
    }

    void drop_table(const std::string& name) {
        // ponytail: removes the catalog entry only; the table's own b-tree
        // pages are left allocated (leaked) rather than walked and freed.
        // A vacuum/compact pass would reclaim them -- unneeded for a demo.
        TableDef& def = get(name);
        tree_.remove(def.table_id);
        sync_root();
        by_name_.erase(name);
    }

    void save(const TableDef& def) {
        tree_.update(def.table_id, encode_table_def(def));
        sync_root();
        by_name_[def.name] = def;
    }

private:
    void sync_root() {
        if (tree_.root() != pager_.catalog_root()) pager_.set_catalog_root(tree_.root());
    }

    Pager& pager_;
    BTree tree_;
    std::unordered_map<std::string, TableDef> by_name_;
};

} // namespace mydb
