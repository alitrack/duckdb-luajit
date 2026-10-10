---
name: duckdb-luajit-tables
description: >
  Generate rows and tables from Lua inside DuckDB (luajit_table), stream with
  coroutine generators, and move complex types (LIST/STRUCT/DECIMAL/BLOB/
  DATE/TIMESTAMP) across the Lua boundary. Use when a query needs rows
  produced programmatically, a custom file parser, or nested-type handling
  that SQL alone cannot express.
version: 0.33.0
---

# duckdb-luajit: table functions & type bridges

## luajit_table — Lua source → rows

```sql
SELECT * FROM luajit_table('return {"a", "b", "c"}');
```

Return a table of tables for typed rows; coroutine.wrap gives a streaming
generator (memory-safe on large outputs):

```lua
-- generator style (inside the source string)
local co = coroutine.wrap(function()
  for i = 1, 1000000 do coroutine.yield({id=i, name="row"..i}) end
end)
return co
```

Named parameters: `list` (path list — replaces io.popen fallback),
`mode := 'blob'` (BLOB return column, NUL-safe).

Platform note: on macOS/Windows run `SET threads=1` (or
`CALL set_max_threads(1)`) before `luajit_table` — serialization requires
serial execution there; Linux is parallel-safe.

## Type bridge cheat sheet

| SQL side | Lua side | Notes |
|---|---|---|
| BIGINT / INTEGER / SMALLINT / TINYINT (+ unsigned) | number | read at true physical width since v0.30 (older builds: garbage for non-BIGINT) |
| DECIMAL(p,s) | number | physical storage bands ≤4→int16, ≤9→int32, ≤18→int64, else int128 |
| LIST(ANY) | Lua table | NULL elements and BOOLEAN supported |
| STRUCT | Lua table | nested recursion, NULL children OK |
| DATE / TIMESTAMP / HUGEINT | bridge types | since v0.20 |
| BLOB | Lua string | `luajit_blob()` scalar (NUL-safe), v0.28 |

## _duckdb_query — SQL from inside Lua

```lua
local rows = _duckdb_query('SELECT count(*) AS n FROM orders')
-- rows is row-major; integer columns arrive at their true width
```

This is the stored-procedure pattern: a registered UDF can query the
database mid-execution. Reentrancy boundary: **nested** `_duckdb_query`
(inside a callback that is itself inside one) hard-errors instead of
deadlocking — restructure rather than nest.

## luajit_blob — binary round-trip

```sql
SELECT luajit_blob('return ...')  -- Lua string ↔ BLOB, NUL bytes preserved
```

Use for hash digests, packed structs, anything with embedded zeros.
