// Self-check for catalog + table layer: constraints, persistence across reopen.
#include "../src/table.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>

using namespace mydb;

int main() {
    const char* path = "/tmp/mydb_test_table.db";
    std::remove(path);

    {
        Pager pager(path);
        Catalog catalog(pager);

        TableDef users_def = catalog.create_table("users", {
            ColumnDef{"id", ColType::INT, true, true, false, false, "", ""},
            ColumnDef{"name", ColType::TEXT, false, true, false, false, "", ""},
            ColumnDef{"email", ColType::TEXT, false, false, true, false, "", ""},
        });
        Table users(pager, catalog, users_def);

        uint64_t r1 = users.insert_row({Value::make_int(1), Value::make_text("Alice"), Value::make_text("a@x.com")});
        uint64_t r2 = users.insert_row({Value::make_int(2), Value::make_text("Bob"), Value::make_text("b@x.com")});
        assert(r1 != r2);
        assert(users.row_count() == 2);

        // NOT NULL violation
        bool threw = false;
        try { users.insert_row({Value::make_int(3), Value::null_of(ColType::TEXT), Value::make_text("c@x.com")}); }
        catch (const std::exception&) { threw = true; }
        assert(threw);

        // UNIQUE violation (duplicate email)
        threw = false;
        try { users.insert_row({Value::make_int(4), Value::make_text("Carl"), Value::make_text("a@x.com")}); }
        catch (const std::exception&) { threw = true; }
        assert(threw);

        // update should not trip UNIQUE against itself
        assert(users.update_row(r1, {Value::make_int(1), Value::make_text("Alice2"), Value::make_text("a@x.com")}));

        TableDef orders_def = catalog.create_table("orders", {
            ColumnDef{"id", ColType::INT, true, true, false, false, "", ""},
            ColumnDef{"user_id", ColType::INT, false, true, false, true, "users", "id"},
            ColumnDef{"amount", ColType::REAL, false, false, false, false, "", ""},
        });
        Table orders(pager, catalog, orders_def);

        // valid FK
        orders.insert_row({Value::make_int(100), Value::make_int(1), Value::make_real(9.99)});
        // invalid FK (no user with id=999)
        threw = false;
        try { orders.insert_row({Value::make_int(101), Value::make_int(999), Value::make_real(1.0)}); }
        catch (const std::exception&) { threw = true; }
        assert(threw);

        assert(orders.row_count() == 1);
    }

    // reopen: catalog + tables must reload from disk correctly
    {
        Pager pager(path);
        Catalog catalog(pager);
        assert(catalog.has_table("users"));
        assert(catalog.has_table("orders"));

        Table users(pager, catalog, catalog.get("users"));
        assert(users.row_count() == 2);
        std::vector<Value> row;
        assert(users.get_row(1, row));
        assert(row[1].s == "Alice2");

        Table orders(pager, catalog, catalog.get("orders"));
        assert(orders.row_count() == 1);

        catalog.drop_table("orders");
        assert(!catalog.has_table("orders"));
    }

    std::printf("test_table: all checks passed\n");
    return 0;
}
