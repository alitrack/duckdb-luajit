---
name: duckdb-luajit-udfs
description: >
  Write Lua-powered scalar UDFs, expressions, and aggregates inside DuckDB SQL
  with the luajit extension. Use when a query needs custom per-row logic,
  string/number transformations, or reusable compiled functions that plain SQL
  or built-in functions don't cover, and the user wants it without leaving SQL
  or adding a Python dependency.
version: 0.33.0
---

# duckdb-luajit: scalar UDFs & expressions

Extension: `luajit` v0.33 (MIT). Install: DuckDB with community extensions,
`INSTALL luajit FROM community; LOAD luajit;` (~1MB, no Python).

## Decision tree (start here)

```text
Compute something once
 → SELECT luajit_i('return <expr using x>', arg)        -- anonymous, use-and-discard
Used it more than twice / hot path?
 → luajit_module(mode := 'quick_compile', sql_name := 'f',
                  source := 'return function(a,b) ... end')
 → then SELECT f(3, 4) like a normal SQL function       -- auto return-type + macro
Anything harder
 → rows from Lua      → luajit_table(...)
 → aggregation        → luajit_module(mode := 'agg', ...)
 → a ready-made lib   → luajit_module(mode := 'install', sql_name := 'json')
 → SQL from inside Lua→ _duckdb_query('SELECT ...') within a UDF
 → native code        → FFI (ffi.load from ~/.duckdb/luajit-ffi/ under 'restricted')
```

## Anonymous expressions (one-off)

`x` binds to the first argument:

```sql
SELECT luajit_i('return x * 2', 21) AS r;                    -- 42
SELECT luajit_s('return "hello " .. x', 'world');            -- hello world
SELECT luajit_i('function(a, b) return a + b end', 20, 22);  -- 42 (multi-arg style)
```

Type suffixes: `luajit_i` (int) / `luajit_f` (float) / `luajit_s` (varchar) /
`luajit_b` (boolean) / `luajit_l` (LIST input, any element type) /
`luajit_vs`/`luajit_vi` (chunk-batched varchar/bigint).

Anonymous source is compiled **per chunk** — fine for one-off use, wrong for
hot paths.

## Registered UDFs (hot paths — compile once per session)

```sql
SELECT * FROM luajit_module(mode := 'compile', sql_name := 'x2',
                            source := 'return function(x) return x * 2 end');
SELECT luajit_i('x2', 21) AS r;                              -- 42
```

`quick_compile` additionally probes the return type and creates a SQL macro,
so the function is callable by its bare name:

```sql
SELECT * FROM luajit_module(mode := 'quick_compile', sql_name := 'x2p1',
                            source := 'return function(x) return x * 2 + 1 end');
SELECT x2p1(5);                                              -- 11
```

Reference throughput (WSL, 1 thread, DuckDB 1.5.5): registered scalar UDF
~36M rows/s, anonymous expression ~30M rows/s. Prefer registration on hot
paths.

## UDF persistence (v0.32+)

Every registration (`compile` / `quick_compile` / `install` / `fennel` /
`load`) is mirrored into a `luajit_udf_registry` table **in the current
database file** and auto-restored on `LOAD luajit`:

```sql
-- session 1
SELECT * FROM luajit_module(mode := 'install', sql_name := 'base64');
-- session 2 (same .db, new process): still works
SELECT luajit_s('base64', 'hello');                          -- aGVsbG8=
```

Scoped per database file (a different .db does NOT inherit UDFs). `drop`
removes one UDF from the registry; `reset` clears all.

## Aggregates

```sql
SELECT * FROM luajit_module(mode := 'agg', sql_name := 'my_agg',
                            source := '... aggregate lua ...');
```

## Gotchas

- NULL in, NULL out: UDFs returning NULL usually mean the Lua function threw
  or returned nil — check `luajit_module(mode := 'last_error')`.
- `luajit_table` serialization on macOS/Windows needs `set_max_threads(1)`;
  Linux runs multi-threaded.
- `quick_compile` auto-detects UDF style: scalar → `luajit_s/i/f/b`,
  batch (table param) → `luajit_vs`.
