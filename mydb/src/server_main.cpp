// server_main.cpp — wires the storage/SQL engine to a REST API and serves
// the click-not-type web app as static files from the same process.
//
// Design note: table/row mutations coming from the web UI are built as
// real AST nodes (InsertStmt/CreateTableStmt/...) and run through
// Database::execute(), the exact same path a typed SQL statement takes
// after parsing. The UI is a click-driven front end over genuine SQL
// execution, not a separate code path.
//
// ponytail: the two row-by-rowid endpoints (PATCH/DELETE on a specific
// row) go straight to Table::update_row/delete_row instead of through
// UpdateStmt/DeleteStmt, because our SQL subset has no ROWID pseudo-column
// to put in a WHERE clause. Constraint checks still run either way; only
// bulk conditional UPDATE/DELETE (via the Advanced SQL box, real WHERE on
// real columns) goes through the parsed-SQL path.
#include "http.h"
#include "sql_executor.h"
#include <ctime>
#include <iostream>

using namespace mydb;

static JsonValue value_to_json(const Value& v) {
    if (v.is_null) return JsonValue::null_();
    switch (v.type) {
        case ColType::INT: return JsonValue::number(static_cast<double>(v.i));
        case ColType::REAL: return JsonValue::number(v.d);
        case ColType::TEXT: return JsonValue::string(v.s);
    }
    return JsonValue::null_();
}

static Value json_to_value(const JsonValue& jv, ColType t) {
    if (jv.is_null()) return Value::null_of(t);
    switch (t) {
        case ColType::INT: return Value::make_int(static_cast<int64_t>(jv.as_number()));
        case ColType::REAL: return Value::make_real(jv.as_number());
        case ColType::TEXT: return Value::make_text(jv.as_string());
    }
    return Value::null_of(t);
}

static ExprPtr json_literal_expr(const JsonValue& jv, ColType t) {
    if (jv.is_null()) return Expr::lit_null();
    switch (t) {
        case ColType::INT: return Expr::lit_int(static_cast<int64_t>(jv.as_number()));
        case ColType::REAL: return Expr::lit_real(jv.as_number());
        case ColType::TEXT: return Expr::lit_str(jv.as_string());
    }
    return Expr::lit_null();
}

static JsonValue column_def_to_json(const ColumnDef& c) {
    JsonValue cj = JsonValue::object();
    cj.set("name", JsonValue::string(c.name));
    cj.set("type", JsonValue::string(col_type_name(c.type)));
    cj.set("primary_key", JsonValue::boolean(c.primary_key));
    cj.set("not_null", JsonValue::boolean(c.not_null));
    cj.set("unique", JsonValue::boolean(c.unique));
    cj.set("has_fk", JsonValue::boolean(c.has_fk));
    if (c.has_fk) {
        cj.set("fk_table", JsonValue::string(c.fk_table));
        cj.set("fk_column", JsonValue::string(c.fk_column));
    }
    return cj;
}

static JsonValue table_def_to_json(const TableDef& def) {
    JsonValue t = JsonValue::object();
    t.set("name", JsonValue::string(def.name));
    JsonValue cols = JsonValue::array();
    for (auto& c : def.columns) cols.push(column_def_to_json(c));
    t.set("columns", cols);
    return t;
}

static JsonValue exec_result_to_json(const ExecResult& r) {
    JsonValue out = JsonValue::object();
    out.set("ok", JsonValue::boolean(r.ok));
    if (!r.ok) { out.set("error", JsonValue::string(r.error)); return out; }
    out.set("message", JsonValue::string(r.message));
    out.set("affected", JsonValue::number(static_cast<double>(r.affected)));
    if (r.last_insert_rowid >= 0) out.set("last_insert_rowid", JsonValue::number(static_cast<double>(r.last_insert_rowid)));
    JsonValue cols = JsonValue::array();
    for (auto& c : r.columns) cols.push(JsonValue::string(c));
    out.set("columns", cols);
    JsonValue rows = JsonValue::array();
    for (auto& row : r.rows) {
        JsonValue rj = JsonValue::array();
        for (auto& v : row) rj.push(value_to_json(v));
        rows.push(rj);
    }
    out.set("rows", rows);
    return out;
}

static std::string now_string() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", std::gmtime(&t));
    return buf;
}

int main(int argc, char** argv) {
    std::string db_path = argc > 1 ? argv[1] : "mydb_data.db";
    int port = argc > 2 ? std::atoi(argv[2]) : 8080;
    std::string web_dir = argc > 3 ? argv[3] : "web";

    Database db(db_path);
    HttpServer server;
    server.serve_static(web_dir);

    // ---- schema browsing ----
    server.get("/api/tables", [&](const HttpRequest&) {
        JsonValue arr = JsonValue::array();
        for (auto& def : db.list_tables()) arr.push(table_def_to_json(def));
        JsonValue out = JsonValue::object(); out.set("tables", arr);
        return HttpResponse::json_ok(out);
    });

    server.get("/api/schema", [&](const HttpRequest&) {
        JsonValue arr = JsonValue::array();
        for (auto& def : db.list_tables()) arr.push(table_def_to_json(def));
        JsonValue out = JsonValue::object();
        out.set("database", JsonValue::string("mydb"));
        out.set("tables", arr);
        return HttpResponse::json_ok(out);
    });

    // ---- AI-agent-friendly export ----
    server.get("/api/export.json", [&](const HttpRequest&) {
        JsonValue tables = JsonValue::array();
        for (auto& def : db.list_tables()) {
            Table t = db.open_table(def.name);
            JsonValue tj = table_def_to_json(def);
            size_t count = 0;
            JsonValue samples = JsonValue::array();
            t.scan([&](uint64_t, const std::vector<Value>& row) {
                count++;
                if (samples.arr.size() < 5) {
                    JsonValue rj = JsonValue::object();
                    for (size_t i = 0; i < def.columns.size(); i++) rj.set(def.columns[i].name, value_to_json(row[i]));
                    samples.push(rj);
                }
                return true;
            });
            tj.set("row_count", JsonValue::number(static_cast<double>(count)));
            tj.set("sample_rows", samples);
            tables.push(tj);
        }
        JsonValue out = JsonValue::object();
        out.set("database", JsonValue::string("mydb"));
        out.set("generated_at", JsonValue::string(now_string()));
        out.set("note", JsonValue::string(
            "Schema + row counts + up to 5 sample rows per table, meant to be pasted into or fetched by an AI agent's context."));
        out.set("tables", tables);
        return HttpResponse::json_ok(out);
    });

    server.get("/api/export.md", [&](const HttpRequest&) {
        std::ostringstream md;
        md << "# mydb schema export\n\nGenerated: " << now_string() << "\n\n";
        for (auto& def : db.list_tables()) {
            Table t = db.open_table(def.name);
            md << "## " << def.name << "\n\n";
            md << "| column | type | constraints |\n|---|---|---|\n";
            for (auto& c : def.columns) {
                std::vector<std::string> flags;
                if (c.primary_key) flags.push_back("PRIMARY KEY");
                if (c.not_null) flags.push_back("NOT NULL");
                if (c.unique) flags.push_back("UNIQUE");
                if (c.has_fk) flags.push_back("REFERENCES " + c.fk_table + "(" + c.fk_column + ")");
                std::string flag_str;
                for (size_t i = 0; i < flags.size(); i++) { if (i) flag_str += ", "; flag_str += flags[i]; }
                md << "| " << c.name << " | " << col_type_name(c.type) << " | " << flag_str << " |\n";
            }
            size_t count = 0;
            std::vector<std::vector<Value>> samples;
            t.scan([&](uint64_t, const std::vector<Value>& row) { count++; if (samples.size() < 5) samples.push_back(row); return true; });
            md << "\nRow count: " << count << "\n\n";
            if (!samples.empty()) {
                md << "Sample rows:\n\n| ";
                for (auto& c : def.columns) md << c.name << " | ";
                md << "\n|"; for (size_t i = 0; i < def.columns.size(); i++) md << "---|";
                md << "\n";
                for (auto& row : samples) {
                    md << "| ";
                    for (auto& v : row) {
                        if (v.is_null) md << "NULL";
                        else if (v.type == ColType::TEXT) md << v.s;
                        else if (v.type == ColType::INT) md << v.i;
                        else md << v.d;
                        md << " | ";
                    }
                    md << "\n";
                }
            }
            md << "\n";
        }
        HttpResponse r; r.content_type = "text/markdown"; r.body = md.str();
        return r;
    });

    // ---- table designer ----
    server.post("/api/tables", [&](const HttpRequest& req) {
        JsonValue body = req.json();
        CreateTableStmt st;
        st.table = body.find("name") ? body.find("name")->as_string() : "";
        if (auto* cols = body.find("columns")) {
            for (auto& cj : cols->arr) {
                ColumnSpec cs;
                cs.name = cj.find("name") ? cj.find("name")->as_string() : "";
                cs.type = cj.find("type") ? cj.find("type")->as_string() : "TEXT";
                cs.primary_key = cj.find("primary_key") && cj.find("primary_key")->as_bool();
                cs.not_null = cj.find("not_null") && cj.find("not_null")->as_bool();
                cs.unique = cj.find("unique") && cj.find("unique")->as_bool();
                std::string fk_table = cj.find("fk_table") ? cj.find("fk_table")->as_string() : "";
                std::string fk_column = cj.find("fk_column") ? cj.find("fk_column")->as_string() : "";
                if (!fk_table.empty() && !fk_column.empty()) { cs.has_fk = true; cs.fk_table = fk_table; cs.fk_column = fk_column; }
                st.columns.push_back(cs);
            }
        }
        ExecResult r = db.execute(Stmt(st));
        HttpResponse resp = HttpResponse::json_ok(exec_result_to_json(r));
        if (!r.ok) resp.status = 400;
        return resp;
    });

    server.del("/api/tables/:name", [&](const HttpRequest& req) {
        DropTableStmt st; st.table = req.params.at("name");
        ExecResult r = db.execute(Stmt(st));
        HttpResponse resp = HttpResponse::json_ok(exec_result_to_json(r));
        if (!r.ok) resp.status = 400;
        return resp;
    });

    // ---- row grid ----
    server.get("/api/tables/:name/rows", [&](const HttpRequest& req) {
        std::string name = req.params.at("name");
        if (!db.catalog().has_table(name)) return HttpResponse::json_error(404, "no such table: " + name);
        TableDef def = db.catalog().get(name);
        Table t = db.open_table(name);
        JsonValue cols = JsonValue::array();
        for (auto& c : def.columns) cols.push(JsonValue::string(c.name));
        JsonValue rows = JsonValue::array();
        t.scan([&](uint64_t rowid, const std::vector<Value>& row) {
            JsonValue rj = JsonValue::object();
            rj.set("rowid", JsonValue::number(static_cast<double>(rowid)));
            JsonValue values = JsonValue::array();
            for (auto& v : row) values.push(value_to_json(v));
            rj.set("values", values);
            rows.push(rj);
            return true;
        });
        JsonValue out = JsonValue::object();
        out.set("columns", cols);
        out.set("rows", rows);
        return HttpResponse::json_ok(out);
    });

    server.post("/api/tables/:name/rows", [&](const HttpRequest& req) {
        std::string name = req.params.at("name");
        if (!db.catalog().has_table(name)) return HttpResponse::json_error(404, "no such table: " + name);
        TableDef def = db.catalog().get(name);
        JsonValue body = req.json();
        const JsonValue* values = body.find("values");
        InsertStmt st; st.table = name;
        std::vector<ExprPtr> row;
        if (values) {
            for (auto& kv : values->obj) {
                int idx = -1;
                for (size_t i = 0; i < def.columns.size(); i++) if (def.columns[i].name == kv.first) idx = static_cast<int>(i);
                if (idx < 0) continue;
                st.columns.push_back(kv.first);
                row.push_back(json_literal_expr(kv.second, def.columns[idx].type));
            }
        }
        st.rows.push_back(row);
        ExecResult r = db.execute(Stmt(st));
        HttpResponse resp = HttpResponse::json_ok(exec_result_to_json(r));
        if (!r.ok) resp.status = 400;
        return resp;
    });

    server.patch_("/api/tables/:name/rows/:rowid", [&](const HttpRequest& req) {
        std::string name = req.params.at("name");
        uint64_t rowid = std::stoull(req.params.at("rowid"));
        if (!db.catalog().has_table(name)) return HttpResponse::json_error(404, "no such table: " + name);
        TableDef def = db.catalog().get(name);
        Table t = db.open_table(name);
        std::vector<Value> row;
        if (!t.get_row(rowid, row)) return HttpResponse::json_error(404, "no such row: " + std::to_string(rowid));
        JsonValue body = req.json();
        if (const JsonValue* values = body.find("values")) {
            for (auto& kv : values->obj) {
                for (size_t i = 0; i < def.columns.size(); i++) {
                    if (def.columns[i].name == kv.first) row[i] = json_to_value(kv.second, def.columns[i].type);
                }
            }
        }
        try {
            t.update_row(rowid, row);
        } catch (const std::exception& e) {
            return HttpResponse::json_error(400, e.what());
        }
        JsonValue out = JsonValue::object(); out.set("ok", JsonValue::boolean(true));
        return HttpResponse::json_ok(out);
    });

    server.del("/api/tables/:name/rows/:rowid", [&](const HttpRequest& req) {
        std::string name = req.params.at("name");
        uint64_t rowid = std::stoull(req.params.at("rowid"));
        if (!db.catalog().has_table(name)) return HttpResponse::json_error(404, "no such table: " + name);
        Table t = db.open_table(name);
        bool ok = t.delete_row(rowid);
        if (!ok) return HttpResponse::json_error(404, "no such row: " + std::to_string(rowid));
        JsonValue out = JsonValue::object(); out.set("ok", JsonValue::boolean(true));
        return HttpResponse::json_ok(out);
    });

    // ---- advanced/raw SQL escape hatch (optional tab in the UI) ----
    server.post("/api/query", [&](const HttpRequest& req) {
        JsonValue body = req.json();
        std::string sql = body.find("sql") ? body.find("sql")->as_string() : "";
        ExecResult r = db.exec_sql(sql);
        HttpResponse resp = HttpResponse::json_ok(exec_result_to_json(r));
        if (!r.ok) resp.status = 400;
        return resp;
    });

    server.listen(port);
    return 0;
}
