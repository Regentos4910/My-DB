# mydb

A relational database built from scratch in C++ (paged file storage, an
on-disk B+tree, a hand-written SQL parser/executor) with a click-not-type
web app, a one-click schema viewer, and a one-click AI-agent-friendly
export. Built as a one-week solo college project.

## Build & run

```sh
make server      # builds ./mydb_server
./mydb_server [db_file] [port] [web_dir]   # defaults: mydb_data.db 8080 web
make test        # runs the three self-check suites (btree, table, sql)
```

Then open `http://localhost:8080` in a browser.

## Architecture

```
src/pager.h          fixed 4KB-page file I/O, free-list page allocator
src/btree.h          on-disk B+tree keyed by rowid (fixed-size leaf slots)
src/row.h            column types + fixed row (de)serialization
src/catalog.h         schema metadata, itself stored in a b-tree
src/table.h          per-table CRUD + constraint checks (PK/UNIQUE/NOT NULL/FK)
src/sql_lexer.h       SQL tokenizer
src/sql_parser.h      recursive-descent parser -> AST
src/sql_executor.h    executes AST (from parsed SQL or built directly by the API) against a Database
src/json.h           minimal JSON value/parser/serializer (hand-rolled, see note below)
src/http.h           minimal single-threaded HTTP/1.1 server over raw sockets
src/server_main.cpp  REST API + static file serving for web/
web/                 plain HTML/CSS/JS click-driven front end
tests/               assert-based self-checks (no framework)
```

One binary, one language, one process: the C++ program embeds the HTTP
server and serves the web app's static files itself.

The web app never asks the user to type SQL for its core flows (browsing
tables, editing cells, creating tables, viewing the schema, exporting for
an agent). Every mutation from the UI is built as a real SQL AST node
(`InsertStmt`, `CreateTableStmt`, ...) and run through the same
`Database::execute()` path a typed SQL statement takes -- the UI is a
front end over genuine SQL execution, not a separate toy code path. A
small "Advanced SQL" tab exists as an optional escape hatch for typing
raw queries directly.

## The three required features

- **Click-not-type UI**: table browser with an editable grid (inline
  cell edit, add/delete row), a table designer (columns, types,
  PK/NOT NULL/UNIQUE checkboxes, FK picker), no SQL box in the main flow.
- **Schema button**: `GET /api/schema` returns the live catalog as JSON;
  the UI renders it both as a reference table and as a dependency-free
  SVG entity-relationship diagram (no charting library, so it still works
  with no internet access).
- **AI-agent export**: `GET /api/export.json` / `GET /api/export.md`
  dump schema + row counts + up to 5 sample rows per table in a format
  meant to be pasted into or fetched by an agent's context. Static
  files today; a live MCP-style wrapper (tools/list, tools/call over the
  same catalog-reading code) is a natural next step, not built here.

## Deliberate simplifications (see `ponytail:` comments in the code)

- **Fixed-size b-tree slots** (500 bytes/row) instead of a variable-length
  slotted page -- simpler, at the cost of a per-row size cap.
- **No delete rebalancing** in the b-tree -- a leaf can go sparse; no
  merge-on-underflow.
- **No secondary indexes** -- PK/UNIQUE/FK checks are full-table scans.
  Fine at college-project row counts.
- **Single writer, no locking, no WAL, no transactions** -- the HTTP
  server is single-threaded and requests are handled one at a time, so
  there's never a second in-flight write to race with.
- **Hand-rolled JSON + HTTP** -- the build sandbox had neither internet
  access nor package-manager root, so `nlohmann/json` and `cpp-httplib`
  (the obvious off-the-shelf picks) couldn't be fetched. Swap them in if
  your environment has them; the surface used here is intentionally small.
- **SQL subset**: CREATE/DROP TABLE, INSERT/SELECT/UPDATE/DELETE, WHERE
  (AND/OR of comparisons), one JOIN, ORDER BY, PK/NOT NULL/UNIQUE/FK. No
  subqueries, aggregates, transactions, or multi-way joins.
- **DROP TABLE leaks pages** instead of walking and freeing them (no
  vacuum/compact tool).
