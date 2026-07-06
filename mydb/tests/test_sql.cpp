// Self-check for the SQL layer: parse + execute the core statement types.
#include "../src/sql_executor.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>

using namespace mydb;

static void must_ok(const ExecResult& r, const char* what) {
    if (!r.ok) { std::fprintf(stderr, "FAILED (%s): %s\n", what, r.error.c_str()); std::abort(); }
}

int main() {
    const char* path = "/tmp/mydb_test_sql.db";
    std::remove(path);
    Database db(path);

    must_ok(db.exec_sql("CREATE TABLE users (id INT PRIMARY KEY, name TEXT NOT NULL, age INT)"), "create users");
    must_ok(db.exec_sql("CREATE TABLE orders (id INT PRIMARY KEY, user_id INT REFERENCES users(id), amount REAL)"), "create orders");

    must_ok(db.exec_sql("INSERT INTO users (id, name, age) VALUES (1, 'Alice', 30)"), "insert alice");
    must_ok(db.exec_sql("INSERT INTO users (id, name, age) VALUES (2, 'Bob', 25)"), "insert bob");
    must_ok(db.exec_sql("INSERT INTO users VALUES (3, 'Carl', 40)"), "insert carl positional");

    // FK violation
    auto bad_fk = db.exec_sql("INSERT INTO orders (id, user_id, amount) VALUES (100, 999, 5.0)");
    assert(!bad_fk.ok);

    must_ok(db.exec_sql("INSERT INTO orders (id, user_id, amount) VALUES (100, 1, 9.99)"), "insert order 1");
    must_ok(db.exec_sql("INSERT INTO orders (id, user_id, amount) VALUES (101, 1, 4.50)"), "insert order 2");
    must_ok(db.exec_sql("INSERT INTO orders (id, user_id, amount) VALUES (102, 2, 20.00)"), "insert order 3");

    // SELECT with WHERE + ORDER BY
    auto sel = db.exec_sql("SELECT id, name FROM users WHERE age >= 30 ORDER BY age DESC");
    must_ok(sel, "select where/order");
    assert(sel.rows.size() == 2);
    assert(sel.rows[0][1].s == "Carl"); // age 40
    assert(sel.rows[1][1].s == "Alice"); // age 30

    // JOIN
    auto joined = db.exec_sql("SELECT users.name, orders.amount FROM orders JOIN users ON orders.user_id = users.id WHERE users.name = 'Alice'");
    must_ok(joined, "join");
    assert(joined.rows.size() == 2);
    for (auto& row : joined.rows) assert(row[0].s == "Alice");

    // UPDATE
    auto upd = db.exec_sql("UPDATE users SET age = 31 WHERE name = 'Alice'");
    must_ok(upd, "update");
    assert(upd.affected == 1);
    auto check = db.exec_sql("SELECT age FROM users WHERE name = 'Alice'");
    assert(check.rows[0][0].i == 31);

    // DELETE
    auto del = db.exec_sql("DELETE FROM orders WHERE amount < 5.0");
    must_ok(del, "delete");
    assert(del.affected == 1);
    auto remaining = db.exec_sql("SELECT id FROM orders");
    assert(remaining.rows.size() == 2);

    // parse error surfaces as ok=false, not a crash
    auto bad = db.exec_sql("SELEKT * FORM nowhere");
    assert(!bad.ok);

    // constraint error surfaces as ok=false
    auto dup = db.exec_sql("INSERT INTO users (id, name, age) VALUES (1, 'Dup', 1)");
    assert(!dup.ok);

    must_ok(db.exec_sql("DROP TABLE orders"), "drop");
    assert(!db.catalog().has_table("orders"));

    std::printf("test_sql: all checks passed\n");
    return 0;
}
