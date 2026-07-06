// sql_lexer.h — tokenizer for the SQL core subset.
#pragma once
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace mydb {

enum class Tok {
    END, IDENT, NUMBER, STRING,
    KW_CREATE, KW_TABLE, KW_DROP, KW_INSERT, KW_INTO, KW_VALUES, KW_SELECT, KW_FROM, KW_WHERE,
    KW_UPDATE, KW_SET, KW_DELETE, KW_JOIN, KW_ON, KW_ORDER, KW_BY, KW_AND, KW_OR, KW_NULL,
    KW_PRIMARY, KW_KEY, KW_FOREIGN, KW_REFERENCES, KW_UNIQUE, KW_NOT, KW_ASC, KW_DESC,
    STAR, COMMA, LPAREN, RPAREN, DOT, SEMI,
    EQ, NEQ, LT, LE, GT, GE
};

struct Token {
    Tok type;
    std::string text;   // identifier text, or raw literal text
    int64_t int_val = 0;
    double real_val = 0;
    bool is_real = false;
};

class SqlError : public std::runtime_error {
public:
    explicit SqlError(const std::string& msg) : std::runtime_error(msg) {}
};

class Lexer {
public:
    explicit Lexer(const std::string& src) : s_(src) { tokenize(); }
    const std::vector<Token>& tokens() const { return toks_; }

private:
    static std::string upper(std::string x) {
        for (auto& c : x) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return x;
    }

    void tokenize() {
        size_t i = 0, n = s_.size();
        while (i < n) {
            char c = s_[i];
            if (std::isspace(static_cast<unsigned char>(c))) { i++; continue; }
            if (c == '\'') {
                size_t j = i + 1; std::string val;
                while (j < n && s_[j] != '\'') {
                    if (s_[j] == '\\' && j + 1 < n) { val += s_[j + 1]; j += 2; }
                    else { val += s_[j]; j++; }
                }
                if (j >= n) throw SqlError("unterminated string literal");
                Token t; t.type = Tok::STRING; t.text = val; toks_.push_back(t);
                i = j + 1; continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c))) {
                size_t j = i; bool is_real = false;
                while (j < n && (std::isdigit(static_cast<unsigned char>(s_[j])) || s_[j] == '.')) {
                    if (s_[j] == '.') is_real = true;
                    j++;
                }
                Token t; t.type = Tok::NUMBER; t.text = s_.substr(i, j - i); t.is_real = is_real;
                if (is_real) t.real_val = std::stod(t.text); else t.int_val = std::stoll(t.text);
                toks_.push_back(t);
                i = j; continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                size_t j = i;
                while (j < n && (std::isalnum(static_cast<unsigned char>(s_[j])) || s_[j] == '_')) j++;
                std::string word = s_.substr(i, j - i);
                Token t; t.text = word;
                std::string up = upper(word);
                if (up == "CREATE") t.type = Tok::KW_CREATE;
                else if (up == "TABLE") t.type = Tok::KW_TABLE;
                else if (up == "DROP") t.type = Tok::KW_DROP;
                else if (up == "INSERT") t.type = Tok::KW_INSERT;
                else if (up == "INTO") t.type = Tok::KW_INTO;
                else if (up == "VALUES") t.type = Tok::KW_VALUES;
                else if (up == "SELECT") t.type = Tok::KW_SELECT;
                else if (up == "FROM") t.type = Tok::KW_FROM;
                else if (up == "WHERE") t.type = Tok::KW_WHERE;
                else if (up == "UPDATE") t.type = Tok::KW_UPDATE;
                else if (up == "SET") t.type = Tok::KW_SET;
                else if (up == "DELETE") t.type = Tok::KW_DELETE;
                else if (up == "JOIN") t.type = Tok::KW_JOIN;
                else if (up == "ON") t.type = Tok::KW_ON;
                else if (up == "ORDER") t.type = Tok::KW_ORDER;
                else if (up == "BY") t.type = Tok::KW_BY;
                else if (up == "AND") t.type = Tok::KW_AND;
                else if (up == "OR") t.type = Tok::KW_OR;
                else if (up == "NULL") t.type = Tok::KW_NULL;
                else if (up == "PRIMARY") t.type = Tok::KW_PRIMARY;
                else if (up == "KEY") t.type = Tok::KW_KEY;
                else if (up == "FOREIGN") t.type = Tok::KW_FOREIGN;
                else if (up == "REFERENCES") t.type = Tok::KW_REFERENCES;
                else if (up == "UNIQUE") t.type = Tok::KW_UNIQUE;
                else if (up == "NOT") t.type = Tok::KW_NOT;
                else if (up == "ASC") t.type = Tok::KW_ASC;
                else if (up == "DESC") t.type = Tok::KW_DESC;
                else { t.type = Tok::IDENT; t.text = word; }
                toks_.push_back(t);
                i = j; continue;
            }
            Token t;
            if (c == '*') { t.type = Tok::STAR; i++; }
            else if (c == ',') { t.type = Tok::COMMA; i++; }
            else if (c == '(') { t.type = Tok::LPAREN; i++; }
            else if (c == ')') { t.type = Tok::RPAREN; i++; }
            else if (c == '.') { t.type = Tok::DOT; i++; }
            else if (c == ';') { t.type = Tok::SEMI; i++; }
            else if (c == '=') { t.type = Tok::EQ; i++; }
            else if (c == '!' && i + 1 < n && s_[i + 1] == '=') { t.type = Tok::NEQ; i += 2; }
            else if (c == '<' && i + 1 < n && s_[i + 1] == '=') { t.type = Tok::LE; i += 2; }
            else if (c == '>' && i + 1 < n && s_[i + 1] == '=') { t.type = Tok::GE; i += 2; }
            else if (c == '<' && i + 1 < n && s_[i + 1] == '>') { t.type = Tok::NEQ; i += 2; }
            else if (c == '<') { t.type = Tok::LT; i++; }
            else if (c == '>') { t.type = Tok::GT; i++; }
            else throw SqlError(std::string("unexpected character: ") + c);
            toks_.push_back(t);
        }
        Token end; end.type = Tok::END; toks_.push_back(end);
    }

    std::string s_;
    std::vector<Token> toks_;
};

} // namespace mydb
