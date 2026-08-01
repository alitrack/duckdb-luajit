# luajit — DuckDB LuaJIT UDF Extension  v1.0.0

Self-contained DuckDB extension for Lua expressions, JIT-compiled UDFs, and nested type bridges via LuaJIT. ~700KB, MIT licensed.

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

-- Inline: function that returns a table
SELECT * FROM luajit_table(
    'return function() local t={};for i=1,100 do t[i]=i*i end;return t end'
);
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
| **`save`** | source? (path) | Persist to file |
| **`load`** | source? (path) | Restore from file |

Result: `(ok BOOLEAN, mode, phase, message, detail, sql_name VARCHAR)`

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
SELECT luajit('return _duckdb_call("CREATE TABLE log(ts TIMESTAMP DEFAULT NOW())")');
```

## Design

- **LuaJIT 2.1** (MIT): ~700KB, self-contained, trace-based JIT
- **11 functions**: 8 scalar, 1 aggregate (GROUP BY), 1 table, 1 module
- **10 modes**: info, compile, quick_compile, inspect, macro, list, drop, reset, save, load
- **Per-group state**: aggregate uses state-pointer-keyed array (256 max groups)
- **Per-type executors**: specialized callbacks for each DuckDB type
- **Nested types**: LIST/STRUCT/MAP bridge via DUCKDB_TYPE_ANY
- **Benchmark**: 50K rows: luajit_i = 0.011s, native = 0.006s (1.9× slower — good for JIT UDF)

## Build

```sh
git clone https://github.com/alitrack/luajit.git && cd luajit
./bootstrap.sh && make release  # → build/release/luajit.duckdb_extension
```

## License

MIT
