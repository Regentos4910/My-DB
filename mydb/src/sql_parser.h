// sql_parser.h — AST + recursive-descent parser for the core SQL subset:
// CREATE/DROP TABLE, INSERT, SELECT (with one JOIN, WHERE, ORDER BY),
// UPDATE, DELETE.
//
// ponytail: no subqueries, no aggregates, no multi-way joins. That is a
// deliberate scope cut agreed up front, not a bug — the "click UI" and the
// AI export both work from this same subset.
#pragma once
#include "sql_lexer.h"
#include <memory>
#include <variant>
#include <vector>

namespace mydb {

enum class ExprKind { LIT_INT, LIT_REAL, LIT_STR, LIT_NULL, COLUMN, AND, OR, CMP };
enum class CmpOp { EQ, NEQ, LT, LE, GT, GE };

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

struct Expr {
    ExprKind kind;
    int64_t i = 0;
    double d = 0;
    std::string s;
    std::string table_qualifier; // for COLUMN, may be empty (unqualified)
    std::string col_name;        // for COLUMN
    ExprPtr left, right;         // for AND/OR/CMP
    CmpOp cmp;                    // for CMP

    static ExprPtr lit_int(int64_t v) { auto e = std::make_shared<Expr>(); e->kind = ExprKind::LIT_INT; e->i = v; return e; }
    static ExprPtr lit_real(double v) { auto e = std::make_shared<Expr>(); e->kind = ExprKind::LIT_REAL; e->d = v; return e; }
    static ExprPtr lit_str(std::string v) { auto e = std::make_shared<Expr>(); e->kind = ExprKind::LIT_STR; e->s = std::move(v); return e; }
    static ExprPtr lit_null() { auto e = std::make_shared<Expr>(); e->kind = ExprKind::LIT_NULL; return e; }
    static ExprPtr column(std::string qualifier, std::string name) {
        auto e = std::make_shared<Expr>(); e->kind = ExprKind::COLUMN;
        e->table_qualifier = std::move(qualifier); e->col_name = std::move(name); return e;
    }
};

struct ColumnSpec {
    std::string name;
    std::string type;
    bool primary_key = false;
    bool not_null = false;
    bool unique = false;
    bool has_fk = false;
    std::string fk_table, fk_column;
};

struct CreateTableStmt { std::string table; std::vector<ColumnSpec> columns; };
struct DropTableStmt { std::string table; };

struct InsertStmt {
    std::string table;
    std::vector<std::string> columns; // empty = all columns, in table-def order
    std::vector<std::vector<ExprPtr>> rows;
};

struct ColRef { std::string table_qualifier; std::string name; }; // name == "*" means all columns

struct JoinClause { std::string table; ExprPtr on; };

struct SelectStmt {
    std::vector<ColRef> columns;
    std::string from_table;
    bool has_join = false;
    JoinClause join;
    ExprPtr where; // nullable
    bool has_order = false;
    std::string order_table_qualifier, order_col;
    bool order_desc = false;
};

struct UpdateStmt {
    std::string table;
    std::vector<std::pair<std::string, ExprPtr>> assignments;
    ExprPtr where;
};

struct DeleteStmt { std::string table; ExprPtr where; };

using Stmt = std::variant<CreateTableStmt, DropTableStmt, InsertStmt, SelectStmt, UpdateStmt, DeleteStmt>;

class Parser {
public:
    explicit Parser(const std::vector<Token>& toks) : toks_(toks) {}

    Stmt parse_statement() {
        Stmt s = parse_one();
        if (peek().type == Tok::SEMI) advance();
        expect(Tok::END, "trailing input after statement");
        return s;
    }

private:
    const Token& peek() const { return toks_[pos_]; }
    const Token& advance() { return toks_[pos_++]; }
    bool check(Tok t) const { return peek().type == t; }
    const Token& expect(Tok t, const std::string& what) {
        if (!check(t)) throw SqlError("expected " + what + " near token index " + std::to_string(pos_));
        return advance();
    }
    std::string expect_ident(const std::string& what) {
        if (!check(Tok::IDENT)) throw SqlError("expected " + what);
        return advance().text;
    }

    Stmt parse_one() {
        if (check(Tok::KW_CREATE)) return parse_create_table();
        if (check(Tok::KW_DROP)) return parse_drop_table();
        if (check(Tok::KW_INSERT)) return parse_insert();
        if (check(Tok::KW_SELECT)) return parse_select();
        if (check(Tok::KW_UPDATE)) return parse_update();
        if (check(Tok::KW_DELETE)) return parse_delete();
        throw SqlError("unrecognized statement");
    }

    CreateTableStmt parse_create_table() {
        advance(); expect(Tok::KW_TABLE, "TABLE");
        CreateTableStmt st; st.table = expect_ident("table name");
        expect(Tok::LPAREN, "(");
        do {
            ColumnSpec c;
            c.name = expect_ident("column name");
            c.type = expect_ident("column type");
            while (check(Tok::KW_PRIMARY) || check(Tok::KW_NOT) || check(Tok::KW_UNIQUE) || check(Tok::KW_REFERENCES)) {
                if (check(Tok::KW_PRIMARY)) { advance(); expect(Tok::KW_KEY, "KEY"); c.primary_key = true; }
                else if (check(Tok::KW_NOT)) { advance(); expect(Tok::KW_NULL, "NULL"); c.not_null = true; }
                else if (check(Tok::KW_UNIQUE)) { advance(); c.unique = true; }
                else if (check(Tok::KW_REFERENCES)) {
                    advance();
                    c.has_fk = true;
                    c.fk_table = expect_ident("referenced table");
                    expect(Tok::LPAREN, "(");
                    c.fk_column = expect_ident("referenced column");
                    expect(Tok::RPAREN, ")");
                }
            }
            st.columns.push_back(c);
        } while (check(Tok::COMMA) && (advance(), true));
        expect(Tok::RPAREN, ")");
        return st;
    }

    DropTableStmt parse_drop_table() {
        advance(); expect(Tok::KW_TABLE, "TABLE");
        DropTableStmt st; st.table = expect_ident("table name");
        return st;
    }

    ExprPtr parse_literal_or_null() {
        if (check(Tok::KW_NULL)) { advance(); return Expr::lit_null(); }
        if (check(Tok::STRING)) { auto v = advance().text; return Expr::lit_str(v); }
        if (check(Tok::NUMBER)) {
            const Token& t = advance();
            return t.is_real ? Expr::lit_real(t.real_val) : Expr::lit_int(t.int_val);
        }
        throw SqlError("expected a literal value");
    }

    InsertStmt parse_insert() {
        advance(); expect(Tok::KW_INTO, "INTO");
        InsertStmt st; st.table = expect_ident("table name");
        if (check(Tok::LPAREN)) {
            advance();
            do { st.columns.push_back(expect_ident("column name")); } while (check(Tok::COMMA) && (advance(), true));
            expect(Tok::RPAREN, ")");
        }
        expect(Tok::KW_VALUES, "VALUES");
        do {
            expect(Tok::LPAREN, "(");
            std::vector<ExprPtr> row;
            do { row.push_back(parse_literal_or_null()); } while (check(Tok::COMMA) && (advance(), true));
            expect(Tok::RPAREN, ")");
            st.rows.push_back(std::move(row));
        } while (check(Tok::COMMA) && (advance(), true));
        return st;
    }

    ColRef parse_colref_or_star() {
        ColRef c;
        if (check(Tok::STAR)) { advance(); c.name = "*"; return c; }
        std::string first = expect_ident("column or table name");
        if (check(Tok::DOT)) {
            advance();
            if (check(Tok::STAR)) { advance(); c.table_qualifier = first; c.name = "*"; return c; }
            c.table_qualifier = first;
            c.name = expect_ident("column name");
        } else {
            c.name = first;
        }
        return c;
    }

    // operand := table.column | column | literal
    ExprPtr parse_operand() {
        if (check(Tok::IDENT)) {
            std::string first = advance().text;
            if (check(Tok::DOT)) { advance(); std::string col = expect_ident("column name"); return Expr::column(first, col); }
            return Expr::column("", first);
        }
        return parse_literal_or_null();
    }

    ExprPtr parse_cmp() {
        ExprPtr left = parse_operand();
        CmpOp op;
        if (check(Tok::EQ)) { op = CmpOp::EQ; advance(); }
        else if (check(Tok::NEQ)) { op = CmpOp::NEQ; advance(); }
        else if (check(Tok::LE)) { op = CmpOp::LE; advance(); }
        else if (check(Tok::GE)) { op = CmpOp::GE; advance(); }
        else if (check(Tok::LT)) { op = CmpOp::LT; advance(); }
        else if (check(Tok::GT)) { op = CmpOp::GT; advance(); }
        else throw SqlError("expected comparison operator");
        ExprPtr right = parse_operand();
        auto e = std::make_shared<Expr>(); e->kind = ExprKind::CMP; e->cmp = op; e->left = left; e->right = right;
        return e;
    }

    ExprPtr parse_and() {
        ExprPtr left = parse_cmp();
        while (check(Tok::KW_AND)) {
            advance();
            ExprPtr right = parse_cmp();
            auto e = std::make_shared<Expr>(); e->kind = ExprKind::AND; e->left = left; e->right = right;
            left = e;
        }
        return left;
    }

    ExprPtr parse_or() {
        ExprPtr left = parse_and();
        while (check(Tok::KW_OR)) {
            advance();
            ExprPtr right = parse_and();
            auto e = std::make_shared<Expr>(); e->kind = ExprKind::OR; e->left = left; e->right = right;
            left = e;
        }
        return left;
    }

    SelectStmt parse_select() {
        advance();
        SelectStmt st;
        do { st.columns.push_back(parse_colref_or_star()); } while (check(Tok::COMMA) && (advance(), true));
        expect(Tok::KW_FROM, "FROM");
        st.from_table = expect_ident("table name");
        if (check(Tok::KW_JOIN)) {
            advance();
            st.has_join = true;
            st.join.table = expect_ident("join table name");
            expect(Tok::KW_ON, "ON");
            st.join.on = parse_or();
        }
        if (check(Tok::KW_WHERE)) { advance(); st.where = parse_or(); }
        if (check(Tok::KW_ORDER)) {
            advance(); expect(Tok::KW_BY, "BY");
            std::string first = expect_ident("order column");
            if (check(Tok::DOT)) { advance(); st.order_table_qualifier = first; st.order_col = expect_ident("order column"); }
            else st.order_col = first;
            st.has_order = true;
            if (check(Tok::KW_DESC)) { advance(); st.order_desc = true; }
            else if (check(Tok::KW_ASC)) { advance(); }
        }
        return st;
    }

    UpdateStmt parse_update() {
        advance();
        UpdateStmt st; st.table = expect_ident("table name");
        expect(Tok::KW_SET, "SET");
        do {
            std::string col = expect_ident("column name");
            expect(Tok::EQ, "=");
            ExprPtr val = parse_literal_or_null();
            st.assignments.push_back({col, val});
        } while (check(Tok::COMMA) && (advance(), true));
        if (check(Tok::KW_WHERE)) { advance(); st.where = parse_or(); }
        return st;
    }

    DeleteStmt parse_delete() {
        advance(); expect(Tok::KW_FROM, "FROM");
        DeleteStmt st; st.table = expect_ident("table name");
        if (check(Tok::KW_WHERE)) { advance(); st.where = parse_or(); }
        return st;
    }

    const std::vector<Token>& toks_;
    size_t pos_ = 0;
};

inline Stmt parse_sql(const std::string& sql) {
    Lexer lex(sql);
    Parser p(lex.tokens());
    return p.parse_statement();
}

} // namespace mydb
