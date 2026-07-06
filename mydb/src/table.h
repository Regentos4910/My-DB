// table.h — a single table's data: b-tree keyed by rowid + constraint
// checks against its ColumnDefs.
//
// ponytail: PK/UNIQUE/FK checks are full-table scans (no secondary
// index exists). O(n) per write is fine at college-project data volumes;
// a real engine would maintain a secondary index for this.
#pragma once
#include "btree.h"
#include "catalog.h"
#include "row.h"
#include <functional>
#include <stdexcept>

namespace mydb {

class Table {
public:
    Table(Pager& pager, Catalog& catalog, TableDef def)
        : pager_(pager), catalog_(catalog), def_(std::move(def)), tree_(pager, def_.root_page) {}

    const TableDef& def() const { return def_; }

    uint64_t insert_row(const std::vector<Value>& values) {
        check_constraints(values, /*self_rowid=*/UINT64_MAX);
        uint64_t rowid = def_.next_rowid++;
        tree_.insert(rowid, encode_row(def_.columns, values));
        persist();
        return rowid;
    }

    bool update_row(uint64_t rowid, const std::vector<Value>& values) {
        check_constraints(values, rowid);
        bool ok = tree_.update(rowid, encode_row(def_.columns, values));
        if (ok) persist();
        return ok;
    }

    bool delete_row(uint64_t rowid) {
        bool ok = tree_.remove(rowid);
        if (ok) persist();
        return ok;
    }

    bool get_row(uint64_t rowid, std::vector<Value>& out) const {
        std::vector<uint8_t> blob;
        if (!tree_.find(rowid, blob)) return false;
        out = decode_row(def_.columns, blob);
        return true;
    }

    // visitor(rowid, values) -> false to stop early
    void scan(const std::function<bool(uint64_t, const std::vector<Value>&)>& visitor) const {
        tree_.scan([&](uint64_t rowid, const std::vector<uint8_t>& blob) {
            return visitor(rowid, decode_row(def_.columns, blob));
        });
    }

    size_t row_count() const {
        size_t n = 0;
        scan([&](uint64_t, const std::vector<Value>&) { n++; return true; });
        return n;
    }

private:
    void check_constraints(const std::vector<Value>& values, uint64_t self_rowid) {
        if (values.size() != def_.columns.size())
            throw std::runtime_error("column count mismatch for table " + def_.name);
        for (size_t i = 0; i < def_.columns.size(); i++) {
            const ColumnDef& col = def_.columns[i];
            const Value& v = values[i];
            if ((col.primary_key || col.not_null) && v.is_null)
                throw std::runtime_error("NOT NULL constraint failed: " + def_.name + "." + col.name);
            if ((col.primary_key || col.unique) && !v.is_null) {
                bool dup = false;
                tree_.scan([&](uint64_t rid, const std::vector<uint8_t>& blob) {
                    if (rid == self_rowid) return true;
                    auto existing = decode_row(def_.columns, blob);
                    if (values_equal(existing[i], v)) { dup = true; return false; }
                    return true;
                });
                if (dup) throw std::runtime_error("UNIQUE constraint failed: " + def_.name + "." + col.name);
            }
            if (col.has_fk && !v.is_null) {
                TableDef& ref_def = catalog_.get(col.fk_table);
                int ref_idx = -1;
                for (size_t j = 0; j < ref_def.columns.size(); j++)
                    if (ref_def.columns[j].name == col.fk_column) ref_idx = static_cast<int>(j);
                if (ref_idx < 0)
                    throw std::runtime_error("FK target column not found: " + col.fk_table + "." + col.fk_column);
                BTree ref_tree(pager_, ref_def.root_page);
                bool found = false;
                ref_tree.scan([&](uint64_t, const std::vector<uint8_t>& blob) {
                    auto rvals = decode_row(ref_def.columns, blob);
                    if (values_equal(rvals[ref_idx], v)) { found = true; return false; }
                    return true;
                });
                if (!found)
                    throw std::runtime_error("FOREIGN KEY constraint failed: " + def_.name + "." + col.name +
                                              " -> " + col.fk_table + "." + col.fk_column);
            }
        }
    }

    void persist() {
        if (tree_.root() != def_.root_page) def_.root_page = tree_.root();
        catalog_.save(def_);
    }

    Pager& pager_;
    Catalog& catalog_;
    TableDef def_;
    BTree tree_;
};

} // namespace mydb
