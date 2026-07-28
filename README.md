# luajit — DuckDB LuaJIT UDF Extension  v0.9

Self-contained DuckDB extension for Lua expressions, JIT-compiled UDFs, and nested type bridges via LuaJIT.

```sql
LOAD luajit;

-- One-shot compile + auto-type + auto-macro
SELECT message FROM luajit_module(
    mode := 'quick_compile',
    source := 'return function(a, b) return a + b end',
    sql_name := 'add'
);
```

## Scalar UDFs (9 functions)

| Function | Input | Output | Use |
|----------|-------|--------|-----|
| `luajit` | VARCHAR | VARCHAR | Inline eval, string UDFs |
| `luajit_i` | VARCHAR + BIGINT... | BIGINT | Integer math |
| `luajit_f` | VARCHAR + DOUBLE... | DOUBLE | Floating-point |
| `luajit_b` | VARCHAR + BOOLEAN... | BOOLEAN | Predicates, filters |
| `luajit_m` | VARCHAR + ANY | DOUBLE | Mixed-type (auto-cast) |
| `luajit_l` | VARCHAR + LIST... | LIST(DOUBLE) | Array operations |
| `luajit_s` | VARCHAR + STRUCT | VARCHAR | Struct operations |
| `luajit_map` | VARCHAR + MAP | VARCHAR | Key-value operations |
| `luajit_agg` | VARCHAR + DOUBLE | DOUBLE | Aggregate (skeleton) |

```sql
SELECT luajit_i('add', 3, 4);                    -- 7 (BIGINT)
SELECT luajit_b('gt0', true);                    -- true (BOOLEAN)
SELECT luajit_l('lsum', [1,2,3]);               -- [6.0] (LIST)
SELECT luajit_s('fmt', {x:3, y:4});             -- 'x=3 y=4 sum=7' (STRUCT)
SELECT luajit_map('fmt_map', map {'a':1});      -- 'a=1' (MAP)
```

## Module API (8 modes)

| Mode | Description |
|------|-------------|
| `info` | Extension version |
| `compile` | Compile + store as global |
| `quick_compile` | One-shot compile + auto-type + auto-macro |
| `inspect` | Show arity, return type, source |
| `macro` | Generate CREATE MACRO DDL |
| `list` | List compiled UDFs |
| `drop` | Remove one UDF |
| `reset` | Clear all UDFs |

Result: `(ok BOOLEAN, mode, phase, message, detail, sql_name VARCHAR)`

## Nested Type Bridges

- **LIST**: `luajit_l` converts DuckDB LIST ↔ Lua table (1-indexed array)
- **STRUCT**: `luajit_s` converts DuckDB STRUCT ↔ Lua table (named keys)
- **MAP**: `luajit_map` converts DuckDB MAP ↔ Lua table (key-value pairs)
- **Activated via** `DUCKDB_TYPE_ANY = 34` (works for scalar function parameters)

## Lua → DuckDB Callback

```sql
SELECT luajit('return _duckdb_call("CREATE TABLE log(ts TIMESTAMP DEFAULT NOW())")');
```

## Design

- **LuaJIT** (MIT): ~700KB, self-contained, trace-based JIT
- **Per-type executors**: 9 specialized callbacks
- **Chunk processing**: row-level Lua calls inside DuckDB data chunks
- **Session management**: list/drop/reset + inspect/probe/auto-macro
- **Nested types**: LIST/STRUCT/MAP bridge via DUCKDB_TYPE_ANY
- **Benchmark**: 50K rows = 1.9x native SQL

## Build

```sh
git clone https://github.com/alitrack/luajit.git && cd luajit
./bootstrap.sh && make release && make test_release
```

## License

MIT
