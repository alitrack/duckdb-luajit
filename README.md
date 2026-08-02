# luajit — DuckDB LuaJIT UDF Extension  v0.19

Self-contained DuckDB extension for Lua expressions, JIT-compiled UDFs, and nested type bridges via LuaJIT. ~700KB, MIT licensed.

> **v0.19 changes**: trusted sandbox mode; `_duckdb_query` result-set bridge;
> DATE/TIMESTAMP/DECIMAL/HUGEINT bridging; streaming `luajit_table` generator
> mode; chunk-batched BIGINT/VARCHAR UDFs (`luajit_vi`/`luajit_vs`); LIST bridge
> NULL elements + BOOLEAN children; MSVC build fix.
>
> **v0.18 changes**: GC64 enabled (2GB memory wall removed); UDF resolution
> moved from per-row to per-chunk (registry refs — ~6× faster row-mode UDFs);
> runtime Lua errors queryable via `luajit_module(mode:='last_error')`; crash
> fix for LIST outputs when the UDF name is undefined.

## Quick Start

```sql
LOAD luajit;

-- Compile + auto-type + auto-macro in one shot
SELECT message FROM luajit_module(
    mode := 'quick_compile',
    source := 'return function(a, b) return a + b end',
    sql_name := 'add'
);
SELECT add(3, 4);  -- 7  (auto-macro → luajit_i)
```

## Functions

### Scalar UDFs (8)

| Function | Type | Example |
|----------|------|---------|
| `luajit(name)` | VARCHAR → VARCHAR | `SELECT luajit('return 1+1')` → `'2'` |
| `luajit_i(name, a..)` | BIGINT → BIGINT | `SELECT luajit_i('add', 3, 4)` → 7 |
| `luajit_f(name, a..)` | DOUBLE → DOUBLE | `SELECT luajit_f('sqrt', 9.0)` → 3.0 |
| `luajit_b(name, a..)` | BOOLEAN → BOOLEAN | `SELECT luajit_b('all', true, false)` → false |
| `luajit_m(name, a..)` | ANY → DOUBLE | `SELECT luajit_m('sum', 1, 2.5, 3)` → 6.5 |
| `luajit_v(name, a..)` | DOUBLE 列批量 | `SELECT luajit_v('dblv', x)` — 1 Lua 调用/chunk，UDF 收整列 table 返 table |
| `luajit_vi(name, a..)` | BIGINT 列批量 | `SELECT luajit_vi('dblv', x)` — int64 版批量 |
| `luajit_vs(name, a..)` | VARCHAR 列批量 | `SELECT luajit_vs('upv', name)` — string 版批量 |
| `luajit_l(name, a..)` | LIST → LIST(DOUBLE) | `SELECT luajit_l('top2', [3,1,4,2])` → `[4.0,3.0]` |
| `luajit_s(name, s)` | STRUCT → VARCHAR | `SELECT luajit_s('fmt', {x:3, y:4})` → `'x=3 y=4'` |
| `luajit_map(name, m)` | MAP → VARCHAR | `SELECT luajit_map('fmt', map{'a':1})` → `'a=1'` |

### Aggregate UDF

```sql
-- Compile a UDF that receives ALL accumulated values as Lua table {v1, v2, ...}
SELECT ok FROM luajit_module(
    mode := 'compile',
    source := 'return function(v) local s=0;for i=1,#v do s=s+v[i] end;return s end',
    sql_name := 'mysum'
);

-- Single group
SELECT luajit_agg('mysum', x) FROM (VALUES (1),(2),(3)) t(x);  -- 6.0

-- GROUP BY support (up to 256 groups)
SELECT g, luajit_agg('mysum', v) FROM data GROUP BY g;

-- median, avg, stddev, percentile — any aggregate you can write in Lua
SELECT luajit_agg('mymedian', price) FROM trades;
```

### Table Function

```sql
-- Compiled UDF
SELECT * FROM luajit_table('gen_data');
-- (1, 'a'), (2, 'b'), (3, 'c')

-- Inline: function that returns a table (materialized at init)
SELECT * FROM luajit_table(
    'return function() local t={};for i=1,100 do t[i]=i*i end;return t end'
);

-- Streaming: function returns a coroutine.wrap generator — O(1) memory,
-- rows are pulled one chunk at a time (yield row_idx, val; nil ends)
SELECT count(*) FROM luajit_table('return function()
    return coroutine.wrap(function()
        for i = 1, 1000000 do coroutine.yield(i, "row-" .. i) end
    end)
end');
```

## Module API (10 modes)

| Mode | Params | Description |
|------|--------|-------------|
| `info` | — | Extension version |
| `compile` | source, sql_name | Compile + store |
| `quick_compile` | source, sql_name | Compile + auto-type + auto-macro |
| `inspect` | sql_name | Arity, return type, source |
| `macro` | sql_name, source? | Generate CREATE MACRO DDL |
| `list` | — | List compiled UDFs |
| `drop` | sql_name | Remove one UDF |
| `reset` | — | Clear all UDFs |
| **`trusted`** | `'on'`/`'off'` | Sandbox toggle: removes io/ffi/package/require/load*/debug, reduces os to date/clock/time/difftime |
| **`last_error`** | — | Most recent Lua UDF runtime error |
| **`save`** | source? (path) | Persist to file |
| **`load`** | source? (path) | Restore from file |

Result: `(ok BOOLEAN, mode, phase, message, detail, sql_name VARCHAR)`

### Error handling

A Lua runtime error in a UDF (e.g. `return x + nil`) marks the affected rows
NULL **and** records the real error message — retrieve it after the query:

```sql
SELECT message FROM luajit_module(mode:='last_error');
-- luajit UDF error: [string "..."]:1: attempt to perform arithmetic on a nil value
```

### Persistence

```sql
-- Save all compiled UDFs
SELECT message FROM luajit_module(mode := 'save', source := 'my_udfs.txt');
-- saved 5 UDFs to my_udfs.txt

-- Restore after restart
SELECT message FROM luajit_module(mode := 'load', source := 'my_udfs.txt');
-- loaded 5 UDFs from my_udfs.txt
```

## Nested Type Bridges

- **LIST**: `luajit_l` — DuckDB LIST ↔ Lua table (1-indexed array)
- **STRUCT**: `luajit_s` — DuckDB STRUCT ↔ Lua table (named keys)
- **MAP**: `luajit_map` — DuckDB MAP ↔ Lua table (key-value pairs)

## Lua → DuckDB Callback

```sql
-- Execute SQL, returns 'ok' or 'error: <msg>'
SELECT luajit('return _duckdb_call("CREATE TABLE log(ts TIMESTAMP DEFAULT NOW())")');

-- Execute query → result table: { {col=val, ...}, ... } (nil on error)
SELECT luajit('
    local rows = _duckdb_query("select id, name from t order by id limit 2")
    return rows[1].id .. "," .. rows[1].name
');
-- Types: integers/doubles → Lua numbers, BOOLEAN → boolean, NULL → nil
-- Errors: returns nil + error message instead of raising (check r == nil)
```

## Design

- **LuaJIT 2.1** (MIT, **GC64 build**): ~700KB, self-contained, trace-based JIT
- **14 functions**: 10 scalar (3 chunk-batched), 1 aggregate (GROUP BY), 1 table, 1 module
- **13 modes**: info, trusted, last_error, compile, quick_compile, inspect, macro, list, drop, reset, save, load
- **Per-group state**: aggregate uses state-pointer-keyed array (256 max groups)
- **Per-type executors**: specialized callbacks for each DuckDB type, UDF resolved once per chunk (registry ref)
- **Nested types**: LIST/STRUCT/MAP bridge via DUCKDB_TYPE_ANY
- **Benchmark** (100K rows): luajit_i = 0.0035s, native = 0.0003s; luajit_v batch ≈ 3.6× native (single-thread)

## Build

```sh
git clone https://github.com/alitrack/luajit.git && cd luajit
./bootstrap.sh && make release  # → build/release/luajit.duckdb_extension
```

## License

MIT
