# luajit — DuckDB LuaJIT UDF Extension  v0.6

Self-contained DuckDB extension for Lua expressions and JIT-compiled UDFs via LuaJIT.

```sql
LOAD luajit;

-- Inline eval
SELECT luajit('return 1 + 2');              -- '3'
SELECT luajit('return _VERSION');            -- 'Lua 5.1'

-- Compile UDF
SELECT ok FROM luajit_module(
    mode := 'compile',
    source := 'return function(a, b) return a + b end',
    sql_name := 'add'
);
```

## Typed UDF API

| Function | Input | Output | Use |
|----------|-------|--------|-----|
| `luajit(expr)` | VARCHAR | VARCHAR | Inline eval, string UDFs |
| `luajit_i(name, ...)` | VARCHAR + varargs BIGINT | BIGINT | Integer math |
| `luajit_f(name, ...)` | VARCHAR + varargs DOUBLE | DOUBLE | Floating-point |
| `luajit_b(name, ...)` | VARCHAR + varargs BOOLEAN | BOOLEAN | Predicates, filters |
| `luajit_m(name, ...)` | VARCHAR + varargs DOUBLE | DOUBLE | Mixed-type (auto-cast) |
| `_duckdb_call(sql)` | Lua global (internal) | status | Call DuckDB SQL from Lua UDFs |

```sql
SELECT luajit_i('add', 3, 4);                -- 7 (BIGINT)
SELECT luajit_f('add', 3::DOUBLE, 4::DOUBLE); -- 7.0 (DOUBLE)
SELECT luajit_b('gt0', 42::BOOLEAN);          -- true (BOOLEAN)
SELECT luajit_m('add', 3, 4::DOUBLE);         -- 7.0 (mixed, auto-cast)
```

## Module API (luajit_module modes)

| Mode | Description |
|------|-------------|
| `info` | Extension version info |
| `compile` | Compile Lua source, store as global |
| `quick_compile` | One-shot compile + auto-probe return type + auto-MACRO |
| `inspect` | Show function arity, return type, source |
| `macro` | Generate `CREATE MACRO` DDL |
| `list` | List all compiled UDFs |
| `drop` | Remove a UDF by name |
| `reset` | Clear all UDFs (fresh Lua state) |

```sql
-- One-shot: compile + auto-type + auto-macro
SELECT detail FROM luajit_module(
    mode := 'quick_compile',
    source := 'return function(a, b) return a + b end',
    sql_name := 'add'
);
--> CREATE OR REPLACE MACRO add(x1, x2) AS luajit_i('add', x1, x2)

-- Inspect function: arity, return type, source
SELECT detail FROM luajit_module(mode := 'inspect', sql_name := 'add');
--> add(2 args) → BIGINT (source: return function(a,b) return a+b end)
```

```sql
-- List
SELECT detail FROM luajit_module(mode := 'list');   -- "add, sq, pos"

-- Drop one
SELECT ok FROM luajit_module(mode := 'drop', sql_name := 'pos');

-- Reset all
SELECT ok FROM luajit_module(mode := 'reset');
```

Result columns: `ok BOOLEAN, mode VARCHAR, phase VARCHAR, message VARCHAR, detail VARCHAR, sql_name VARCHAR`

## MACRO Sugar

```sql
SELECT message FROM luajit_module(
    mode := 'macro', sql_name := 'add', source := 'i'
);
--> CREATE OR REPLACE MACRO add(x1,x2) AS luajit_i('add', x1, x2)
```

## Lua→DuckDB Callback

Lua UDFs can call DuckDB SQL via `_duckdb_call(sql)`:

```sql
SELECT luajit('return _duckdb_call("CREATE TABLE log(ts TIMESTAMP DEFAULT NOW())")');
SELECT luajit('return _duckdb_call("INSERT INTO log DEFAULT VALUES")');
```

This enables UDFs that write audit logs, maintain counters, or interact with DuckDB's SQL engine from within Lua.

## Design

- **LuaJIT** (MIT): trace-based JIT, ~700KB, self-contained
- **Per-type executors**: BIGINT/DOUBLE/BOOLEAN/MIXED avoid string conversion
- **Chunk processing**: DuckDB feeds data chunks; row-by-row Lua calls inside
- **Session management**: list/drop/reset for UDF lifecycle
- **FFI bridge**: `_duckdb_call` exposes DuckDB query execution to Lua

## Build

```sh
git clone https://github.com/alitrack/luajit.git && cd luajit
./bootstrap.sh && make release && make test_release
```

## License

MIT
