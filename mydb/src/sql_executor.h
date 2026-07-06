// sql_executor.h — executes parsed SQL statements (or hand-built AST nodes
// from the REST layer -- same code path either way) against a Database.
//
// ponytail: WHERE/JOIN-ON supports AND/OR of simple comparisons only, no
// NULL-aware three-valued logic (a comparison against NULL is just treated
// as false). Nested-loop join, no query planner -- fine at the row counts
// a college demo will ever hold.
#pragma once
#include "catalog.h"
#include "sql_parser.h"
#include "table.h"
#include <algorithm>
#include <memory>
#include <variant>

namespace mydb {

struct ExecResult {
    bool ok = true;
    std::string error;
    std::string message;
    int64_t affected = 0;
    int64_t last_insert_rowid = -1;
    std::vector<std::string> columns;      // populated for SELECT
    std::vector<std::vector<Value>> rows;  // populated for SELECT
};

class Database {
public:
    explicit Database(const std::string& path) : pager_(path), catalog_(pager_) {}

    Catalog& catalog() { return catalog_; }

    std::vector<TableDef> list_tables() {
        std::vector<TableDef> out;
        for (auto& name : catalog_.table_names()) out.push_back(catalog_.get(name));
        return out;
    }

    Table open_table(const std::string& name) { return Table(pager_, catalog_, catalog_.get(name)); }

    ExecResult execute(const Stmt& stmt) {
        ExecResult r;
        try {
            std::visit([&](auto&& s) { execute_one(s, r); }, stmt);
        } catch (const std::exception& e) {
            r.ok = false; r.error = e.what();
        }
        return r;
    }

    ExecResult exec_sql(const std::string& sql) {
        try {
            return execute(parse_sql(sql));
        } catch (const std::exception& e) {
            ExecResult r; r.ok = false; r.error = e.what(); return r;
        }
    }

private:
    struct RowContext {
        std::string from_name; const std::vector<ColumnDef>* from_cols; const std::vector<Value>* from_row;
        bool has_join = false; std::string join_name; const std::vector<ColumnDef>* join_cols = nullptr; const std::vector<Value>* join_row = nullptr;

        static int find_col(const std::vector<ColumnDef>& cols, const std::string& name) {
            for (size_t i = 0; i < cols.size(); i++) if (cols[i].name == name) return static_cast<int>(i);
            return -1;
        }

        Value lookup(const std::string& qualifier, const std::string& name) const {
            if (!qualifier.empty()) {
                if (qualifier == from_name) {
                    int idx = find_col(*from_cols, name);
                    if (idx < 0) throw std::runtime_error("no such column: " + qualifier + "." + name);
                    return (*from_row)[idx];
                }
                if (has_join && qualifier == join_name) {
                    int idx = find_col(*join_cols, name);
                    if (idx < 0) throw std::runtime_error("no such column: " + qualifier + "." + name);
                    return (*join_row)[idx];
                }
                throw std::runtime_error("unknown table qualifier: " + qualifier);
            }
            int idx = find_col(*from_cols, name);
            if (idx >= 0) return (*from_row)[idx];
            if (has_join) {
                idx = find_col(*join_cols, name);
                if (idx >= 0) return (*join_row)[idx];
            }
            throw std::runtime_error("no such column: " + name);
        }
    };

    static Value bool_val(bool b) { return Value::make_int(b ? 1 : 0); }
    static bool truthy(const Value& v) { return !v.is_null && v.i != 0; }

    static Value eval_expr(const ExprPtr& e, const RowContext& ctx) {
        switch (e->kind) {
            case ExprKind::LIT_INT: return Value::make_int(e->i);
            case ExprKind::LIT_REAL: return Value::make_real(e->d);
            case ExprKind::LIT_STR: return Value::make_text(e->s);
            case ExprKind::LIT_NULL: { Value v; v.is_null = true; return v; }
            case ExprKind::COLUMN: return ctx.lookup(e->table_qualifier, e->col_name);
            case ExprKind::CMP: {
                Value l = eval_expr(e->left, ctx), r = eval_expr(e->right, ctx);
                if (l.is_null || r.is_null) return bool_val(false);
                bool res = false;
                switch (e->cmp) {
                    case CmpOp::EQ: res = values_equal(l, r); break;
                    case CmpOp::NEQ: res = !values_equal(l, r); break;
                    case CmpOp::LT: res = compare_values(l, r) < 0; break;
                    case CmpOp::LE: res = compare_values(l, r) <= 0; break;
                    case CmpOp::GT: res = compare_values(l, r) > 0; break;
                    case CmpOp::GE: res = compare_values(l, r) >= 0; break;
                }
                return bool_val(res);
            }
            case ExprKind::AND: return bool_val(truthy(eval_expr(e->left, ctx)) && truthy(eval_expr(e->right, ctx)));
            case ExprKind::OR: return bool_val(truthy(eval_expr(e->left, ctx)) || truthy(eval_expr(e->right, ctx)));
        }
        throw std::runtime_error("unreachable expr kind");
    }

    static Value eval_literal(const ExprPtr& e, ColType target_type) {
        if (e->kind == ExprKind::LIT_NULL) return Value::null_of(target_type);
        if (e->kind == ExprKind::LIT_INT) {
            if (target_type == ColType::REAL) return Value::make_real(static_cast<double>(e->i));
            if (target_type == ColType::TEXT) throw std::runtime_error("type mismatch: expected TEXT, got a number");
            return Value::make_int(e->i);
        }
        if (e->kind == ExprKind::LIT_REAL) {
            if (target_type != ColType::REAL) throw std::runtime_error("type mismatch: expected " + col_type_name(target_type) + ", got a real number");
            return Value::make_real(e->d);
        }
        if (e->kind == ExprKind::LIT_STR) {
            if (target_type != ColType::TEXT) throw std::runtime_error("type mismatch: expected " + col_type_name(target_type) + ", got text");
            return Value::make_text(e->s);
        }
        throw std::runtime_error("expected a literal value");
    }

    void execute_one(const CreateTableStmt& st, ExecResult& r) {
        std::vector<ColumnDef> cols;
        for (auto& c : st.columns) {
            ColumnDef cd;
            cd.name = c.name;
            cd.type = col_type_from_name(c.type);
            cd.primary_key = c.primary_key;
            cd.not_null = c.not_null;
            cd.unique = c.unique;
            cd.has_fk = c.has_fk;
            cd.fk_table = c.fk_table;
            cd.fk_column = c.fk_column;
            cols.push_back(cd);
        }
        catalog_.create_table(st.table, cols);
        r.message = "table '" + st.table + "' created";
    }

    void execute_one(const DropTableStmt& st, ExecResult& r) {
        catalog_.drop_table(st.table);
        r.message = "table '" + st.table + "' dropped";
    }

    void execute_one(const InsertStmt& st, ExecResult& r) {
        TableDef def = catalog_.get(st.table);
        Table table(pager_, catalog_, def);
        std::vector<int> col_index;
        if (st.columns.empty()) {
            for (size_t i = 0; i < def.columns.size(); i++) col_index.push_back(static_cast<int>(i));
        } else {
            for (auto& name : st.columns) {
                int idx = RowContext::find_col(def.columns, name);
                if (idx < 0) throw std::runtime_error("no such column: " + name);
                col_index.push_back(idx);
            }
        }
        int64_t last_id = -1;
        for (auto& row_exprs : st.rows) {
            if (row_exprs.size() != col_index.size()) throw std::runtime_error("value count does not match column count");
            std::vector<Value> values(def.columns.size());
            for (size_t i = 0; i < def.columns.size(); i++) values[i] = Value::null_of(def.columns[i].type);
            for (size_t k = 0; k < row_exprs.size(); k++)
                values[col_index[k]] = eval_literal(row_exprs[k], def.columns[col_index[k]].type);
            last_id = static_cast<int64_t>(table.insert_row(values));
        }
        r.affected = static_cast<int64_t>(st.rows.size());
        r.last_insert_rowid = last_id;
        r.message = std::to_string(st.rows.size()) + " row(s) inserted";
    }

    void execute_one(const SelectStmt& st, ExecResult& r) {
        TableDef from_def = catalog_.get(st.from_table);
        Table from_table(pager_, catalog_, from_def);
        TableDef join_def;
        std::unique_ptr<Table> join_table;
        if (st.has_join) {
            join_def = catalog_.get(st.join.table);
            join_table = std::make_unique<Table>(pager_, catalog_, join_def);
        }

        struct ProjCol { std::string label, qualifier, name; };
        std::vector<ProjCol> proj;
        for (auto& c : st.columns) {
            if (c.name == "*") {
                if (!c.table_qualifier.empty()) {
                    const std::vector<ColumnDef>* cols = nullptr;
                    if (c.table_qualifier == st.from_table) cols = &from_def.columns;
                    else if (st.has_join && c.table_qualifier == st.join.table) cols = &join_def.columns;
                    else throw std::runtime_error("unknown table: " + c.table_qualifier);
                    for (auto& cd : *cols) proj.push_back({cd.name, c.table_qualifier, cd.name});
                } else {
                    for (auto& cd : from_def.columns) proj.push_back({cd.name, st.from_table, cd.name});
                    if (st.has_join) for (auto& cd : join_def.columns) proj.push_back({cd.name, st.join.table, cd.name});
                }
            } else {
                std::string label = c.table_qualifier.empty() ? c.name : (c.table_qualifier + "." + c.name);
                proj.push_back({label, c.table_qualifier, c.name});
            }
        }
        for (auto& p : proj) r.columns.push_back(p.label);

        std::vector<std::pair<std::vector<Value>, std::vector<Value>>> combos;
        from_table.scan([&](uint64_t, const std::vector<Value>& from_row) {
            if (st.has_join) {
                join_table->scan([&](uint64_t, const std::vector<Value>& join_row) {
                    RowContext ctx{st.from_table, &from_def.columns, &from_row, true, st.join.table, &join_def.columns, &join_row};
                    if (truthy(eval_expr(st.join.on, ctx)) && (!st.where || truthy(eval_expr(st.where, ctx))))
                        combos.push_back({from_row, join_row});
                    return true;
                });
            } else {
                RowContext ctx{st.from_table, &from_def.columns, &from_row, false, "", nullptr, nullptr};
                if (!st.where || truthy(eval_expr(st.where, ctx))) combos.push_back({from_row, {}});
            }
            return true;
        });

        if (st.has_order) {
            std::sort(combos.begin(), combos.end(), [&](auto& a, auto& b) {
                RowContext ca{st.from_table, &from_def.columns, &a.first, st.has_join, st.has_join ? st.join.table : "",
                              st.has_join ? &join_def.columns : nullptr, st.has_join ? &a.second : nullptr};
                RowContext cb{st.from_table, &from_def.columns, &b.first, st.has_join, st.has_join ? st.join.table : "",
                              st.has_join ? &join_def.columns : nullptr, st.has_join ? &b.second : nullptr};
                int c = compare_values(ca.lookup(st.order_table_qualifier, st.order_col), cb.lookup(st.order_table_qualifier, st.order_col));
                return st.order_desc ? c > 0 : c < 0;
            });
        }

        for (auto& combo : combos) {
            RowContext ctx{st.from_table, &from_def.columns, &combo.first, st.has_join, st.has_join ? st.join.table : "",
                          st.has_join ? &join_def.columns : nullptr, st.has_join ? &combo.second : nullptr};
            std::vector<Value> out_row;
            for (auto& p : proj) out_row.push_back(ctx.lookup(p.qualifier, p.name));
            r.rows.push_back(std::move(out_row));
        }
        r.affected = static_cast<int64_t>(r.rows.size());
    }

    void execute_one(const UpdateStmt& st, ExecResult& r) {
        TableDef def = catalog_.get(st.table);
        Table table(pager_, catalog_, def);
        std::vector<uint64_t> targets;
        table.scan([&](uint64_t rowid, const std::vector<Value>& row) {
            RowContext ctx{st.table, &def.columns, &row, false, "", nullptr, nullptr};
            if (!st.where || truthy(eval_expr(st.where, ctx))) targets.push_back(rowid);
            return true;
        });
        int64_t count = 0;
        for (uint64_t rowid : targets) {
            std::vector<Value> row;
            table.get_row(rowid, row);
            for (auto& asg : st.assignments) {
                int idx = RowContext::find_col(def.columns, asg.first);
                if (idx < 0) throw std::runtime_error("no such column: " + asg.first);
                row[idx] = eval_literal(asg.second, def.columns[idx].type);
            }
            table.update_row(rowid, row);
            count++;
        }
        r.affected = count;
        r.message = std::to_string(count) + " row(s) updated";
    }

    void execute_one(const DeleteStmt& st, ExecResult& r) {
        TableDef def = catalog_.get(st.table);
        Table table(pager_, catalog_, def);
        std::vector<uint64_t> targets;
        table.scan([&](uint64_t rowid, const std::vector<Value>& row) {
            RowContext ctx{st.table, &def.columns, &row, false, "", nullptr, nullptr};
            if (!st.where || truthy(eval_expr(st.where, ctx))) targets.push_back(rowid);
            return true;
        });
        for (uint64_t rowid : targets) table.delete_row(rowid);
        r.affected = static_cast<int64_t>(targets.size());
        r.message = std::to_string(targets.size()) + " row(s) deleted";
    }

    Pager pager_;
    Catalog catalog_;
};

} // namespace mydb
