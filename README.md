# Scratch DB

Scratch DB is a zero-dependency relational database written in C++.

It includes a hand-written SQL parser, typed schemas, row serialization, slotted pages, a buffer/page cache, B+ tree index files, constraints, transactions, WAL recovery, a CLI, an HTTP server, and optional in-memory tables.

The project intentionally avoids third-party runtime libraries. It uses C++17, the standard library, POSIX file/socket APIs, and CMake.

## Quick Start

Needs CMake support!

Build:

```bash
make build
```

Run the CLI:

```bash
make run
```

Run tests:

```bash
make test
```

Run the HTTP server:

```bash
cmake --build build
./build/db_server --host 0.0.0.0 --port 8080 --data data
```

Then open:

```text
http://localhost:8080
```

## Example SQL

```sql
CREATE DATABASE demo;

CREATE TABLE users (
    id INTEGER NOT NULL PRIMARY KEY,
    name VARSTRING(64) NOT NULL,
    joined_on DATE NOT NULL
);

INSERT INTO users VALUES
    (1, 'alice', '2026-01-10'),
    (2, 'bob', '2026-01-11');

CREATE INDEX idx_users_id ON users (id);

SELECT * FROM users WHERE id = 1;
```

Memory-backed tables are also supported:

```sql
CREATE TABLE cache (
    id INTEGER NOT NULL,
    value TEXT
) ENGINE = MEMORY;

INSERT INTO cache VALUES (1, 'hot data');
SELECT * FROM cache;
```

If `ENGINE` is omitted, tables use the normal disk-backed storage path.

## SQL Features

Supported statement families include:

- `CREATE DATABASE`, `DROP DATABASE`, `USE`
- `CREATE TABLE`, `CREATE TABLE IF NOT EXISTS`, `DROP TABLE`
- `ALTER TABLE ADD ...`, `ALTER TABLE DROP CONSTRAINT ...`
- `CREATE INDEX`, `CREATE UNIQUE INDEX`, `DROP INDEX`, `SHOW INDEXES`
- `SHOW DATABASES`, `SHOW TABLES`, `DESCRIBE`
- `INSERT`, multi-row `INSERT`, `INSERT ... SELECT`
- `SELECT`, `WHERE`, `JOIN`, chained joins, derived tables, subqueries
- `WITH`, multiple CTEs
- `UNION`, `INTERSECT`, `ALL`, `SOME`
- `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT`
- `UPDATE`, `DELETE`
- explicit `BEGIN`, `COMMIT`, `ROLLBACK`

The parser is intentionally strict:

- SQL keywords must be uppercase.
- table and column names must be lowercase identifiers.
- statements must end with semicolons in the CLI.

## Data Types

Current column types:

- `INTEGER`
- `CHAR`
- `STRING(n)`
- `VARSTRING(n)`
- `NUMBER(p, s)`
- `DATE`
- `TIME`
- `DATETIME`
- `TEXT`

`STRING(n)` is fixed-size logical text. `VARSTRING(n)` is variable-size text with a user limit. `TEXT` is larger variable text with a system-defined limit.

## Constraints

Scratch DB supports:

- `NOT NULL`
- `PRIMARY KEY`
- `UNIQUE`
- `REFERENCES table(column)`
- simple `CHECK` comparisons

Constraints receive generated ids in schema metadata so they can be referenced later by `ALTER TABLE DROP CONSTRAINT`.

## HTTP API

The HTTP server is meant for demos and simple app frontends.

Health check:

```http
GET /health
```

Run SQL:

```http
POST /query
Content-Type: application/json

{
  "session_id": "judge_1",
  "query": "SELECT * FROM users;"
}
```

Responses are structured JSON:

```json
{
  "ok": true,
  "session_id": "judge_1",
  "columns": [
    { "name": "id", "type": "integer" },
    { "name": "name", "type": "varstring" }
  ],
  "rows": [
    ["1", "alice"]
  ],
  "metadata": {
    "row_count": 1,
    "message": ""
  }
}
```

Each `session_id` owns its own `QueryExecutor`, so one client can keep a selected database or an open transaction without overwriting another client's session state.

## Architecture

Scratch DB is split into a few layers.

```text
CLI / HTTP server
        |
Query parser
        |
Query executor
        |
Database facade
        |
Catalog + storage + indexes + WAL + locks
```

### Parser

The parser is hand-written in C++. It tokenizes SQL, validates the project's casing rules, and produces a `ParsedQuery` structure. It does not depend on ANTLR, yacc, bison, or parser-generator tooling.

### Executor

The executor receives parsed queries and returns `QueryResult` objects. It does not directly print query results, which keeps the engine usable from both the CLI and HTTP server.

`QueryResult` contains:

- column metadata
- rows
- row count and message metadata
- structured error details with token and position

### Catalog

The catalog stores database and table metadata on disk.

Each database is a directory. Each table is a directory containing:

- `schema.catalog`
- `data.tbl`
- zero or more index files such as `index_1.idx`

`schema.catalog` stores:

- table name
- storage mode: `DISK` or `MEMORY`
- columns
- constraints
- indexes

Schema writes are done with a temporary file, flush, rename, and parent-directory sync.

### Row And Record Layer

Rows are represented as typed `Value` objects. Disk-backed tables use custom file type, serializing rows into compact record bytes before writing them into pages.

Variable-size values are represented with fixed-size metadata in the record body and payload bytes after the fixed section.

### Page Storage

Disk tables use slotted pages. Records are written from the end of the page toward the header/slot area. The slot directory grows from the front. Deleted or moved rows may create holes, and page compaction can rebuild contiguous free space.

`TableFile` owns a persistent file descriptor and exposes record operations over pages:

- insert
- read
- update
- delete
- scan

### Page Cache

The page cache stores table pages keyed by table file and `page_id`.

Each cached frame tracks:

- page data
- dirty flag
- pin count
- replacement metadata

Dirty table pages are flushed only after the WAL is durable through the page's LSN.

### Indexes

Indexes are stored in separate `.idx` files under the table directory. The current index structure is a B+ tree with header, internal, and leaf pages.

Indexes support:

- equality lookup
- range lookup
- unique indexes
- use in simple `WHERE` predicates
- use in simple equality joins

For disk tables, index page changes are WAL-logged as raw page images. For memory tables, index metadata is kept, but lookups and uniqueness checks are served by scanning the in-memory rows so stale persistent index files are not used for RAM-only data.

### WAL And Recovery

Scratch DB uses an append-only write-ahead log per database:

```text
database.wal
```

The WAL records:

- transaction begin
- page before/after images
- schema changes
- table creation/drop
- commit
- abort

Recovery replays committed history and undoes transactions that started but did not commit or abort. Compensation records are written during undo.

Table pages and index pages share the same WAL stream. Table page recovery restores the page image and updates the table page LSN. Index page recovery writes raw page images without interpreting them as table pages.

### Transactions And Concurrency

Explicit transactions are supported:

```sql
BEGIN;
INSERT INTO users VALUES (3, 'carol', '2026-01-12');
COMMIT;
```

or:

```sql
BEGIN;
UPDATE users SET name = 'temp' WHERE id = 1;
ROLLBACK;
```

Concurrency is intentionally simple and conservative:

- table-level locks
- shared locks for reads
- exclusive locks for writes
- locks held to transaction end for explicit transactions
- statement-duration locking outside explicit transactions

This gives serializable table-level behavior. It is not row-level locking, and deadlock detection is still future work.

If a modifying statement fails inside an explicit transaction, the transaction is marked aborted. After that, later writes and `COMMIT` fail; `ROLLBACK` is required.

### Memory Tables

`ENGINE = MEMORY` tables store row data in RAM.

The schema is still stored in the catalog, but rows are kept in an in-process memory store. This means:

- SQL behavior is the same for `SELECT`, `INSERT`, `UPDATE`, and `DELETE`.
- rows are visible to other `Database` objects in the same running process.
- row data is gone when the process exits.
- the table definition remains because the catalog is durable.

Memory tables are useful for cache-like demo workloads where persistence is not needed.

## Project Layout

```text
include/
  catalog/       column and schema metadata
  concurrency/   table lock manager
  database/      high-level database facade
  db_types/      date/time/datetime types
  query/         parser, executor, result types
  record/        row, value, serializer
  storage/       pages, table files, indexes, WAL, page cache

src/
  cli/           table printer
  catalog/
  concurrency/
  database/
  db_types/
  query/
  record/
  storage/
  main.cpp       interactive CLI
  server.cpp     HTTP server

tests/           zero-dependency test binaries
```

## Testing

The test suite covers:

- page layout and compaction
- table file operations
- page cache behavior
- B+ tree index behavior
- WAL and crash recovery
- catalog persistence
- row serialization
- parser behavior and errors
- query execution
- locks and transaction behavior
- HTTP server sessions and transaction behavior

Run everything:

```bash
make test
```

In restricted sandboxes, the HTTP server test may need permission to open localhost sockets.

## Current Limits

Scratch DB is still a learning database, not a production database.

Known limits:

- table-level locking only
- no deadlock detector yet (timed mutex to prevent them)
- no cost-based optimizer using relational algebra
- HTTP server is demo-oriented

## Live Preview

The live version is written in react for frontend but it calls this cpp db on its backend which is in repo: https://github.com/cryptox-prog/scratch-db-online-cli and available at https://scratch-db-online-cgc1dp3m4-argonauts1.vercel.app/
