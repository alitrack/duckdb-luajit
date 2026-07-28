# luajit — DuckDB LuaJIT UDF Extension

Self-contained DuckDB extension for Lua expressions and JIT-compiled UDFs via LuaJIT.
**~700KB, zero external dependencies, cross-platform.**

```sql
LOAD luajit;

-- Inline Lua evaluation (returns VARCHAR)
SELECT luajit('return 1 + 2');                            -- '3'
SELECT luajit('return _VERSION');                          -- 'Lua 5.1'

-- Compile UDF as a named Lua global
SELECT ok FROM luajit_module(
    mode := 'compile',
    source := 'return function(a, b) return a + b end',
    sql_name := 'add'
);

-- Typed call (BIGINT → BIGINT)
SELECT luajit_i('add', 3, 4);                              -- 7

-- Typed call (DOUBLE → DOUBLE)
SELECT luajit_f('add', 3::DOUBLE, 4::DOUBLE);              -- 7.0

-- String call (VARCHAR → VARCHAR)
SELECT luajit('return add(3, 4)');                         -- '7'
```

## Typed UDF API

| Function | Input | Output | Use |
|----------|-------|--------|-----|
| `luajit(expr)` | VARCHAR | VARCHAR | Inline eval, string UDFs |
| `luajit_i(name, ...)` | VARCHAR + varargs BIGINT | BIGINT | Integer math UDFs |
| `luajit_f(name, ...)` | VARCHAR + varargs DOUBLE | DOUBLE | Floating-point UDFs |
| `luajit_module(mode:='info')` | — | table | Extension version |
| `luajit_module(mode:='compile', source, sql_name)` | — | table | Store UDF as Lua global |
| `luajit_module(mode:='macro', sql_name, source)` | — | table | Generate `CREATE MACRO` DDL |

## MACRO Sugar

Turn any compiled UDF into a first-class SQL function:

```sql
-- Compile
SELECT ok FROM luajit_module(
    mode := 'compile',
    source := 'return function(x) return x * x end',
    sql_name := 'sq'
);

-- Generate CREATE MACRO DDL (auto-detects arity)
SELECT message FROM luajit_module(
    mode := 'macro',
    sql_name := 'sq',
    source := 'i'        -- 'i'=BIGINT, 'f'=DOUBLE, default VARCHAR
);
--> CREATE OR REPLACE MACRO sq(x1) AS luajit_i('sq', x1)

-- Execute the DDL, then call naturally
SELECT sq(5);                                              -- 25
```

## Table Data

```sql
CREATE TABLE t AS SELECT * FROM (VALUES (1), (2), (3)) t(v);

-- Compile a UDF
SELECT ok FROM luajit_module(
    mode := 'compile',
    source := 'return function(x) return x * 10 end',
    sql_name := 'mul10'
);

-- Apply to every row (typed BIGINT)
SELECT v, luajit_i('mul10', v) FROM t ORDER BY v;
--  (1, 10), (2, 20), (3, 30)

-- String concatenation via luajit()
SELECT v, luajit('return mul10(' || v || ')') FROM t;
```

## Advanced: Multiple UDFs

UDFs are stored as Lua globals — per-function dispatch is automatic:

```sql
SELECT ok FROM luajit_module(mode:='compile',
    source:='return function(s) return string.upper(s) end', sql_name:='up');
SELECT ok FROM luajit_module(mode:='compile',
    source:='return function(x) return x * 2 end', sql_name:='dbl');

-- Both work correctly (no cross-UDF pollution)
SELECT luajit('return up("hello")'), luajit('return dbl(21)');
-- 'HELLO', '42'
```

## Design

- **LuaJIT** (MIT): trace-based JIT compiles hot paths to native code
- **UDFs as Lua globals**: no per-function DuckDB registration overhead, no extra_info issues
- **Varargs**: `luajit_i`/`luajit_f` use `duckdb_scalar_function_set_varargs` for arbitrary arity
- **Self-contained**: ~700KB, LuaJIT statically linked, no system Lua required
- **Cross-platform**: Linux / macOS / Windows via native build; WASM skeleton provided

## Build

```sh
git clone https://github.com/alitrack/luajit.git
cd luajit
./bootstrap.sh        # clones extension-ci-tools + LuaJIT
make release
make test_release
```

## Limitations (v0.4)

- UDF args/return are all the same type per variant (all-BIGINT or all-DOUBLE)
- No nested types (LIST, STRUCT, MAP) — string-only for complex data
- No persistent UDF storage across sessions (rebuild on each LOAD)
- WASM builds compile but without Lua (stub mode); PUC Lua or interpreter-mode LuaJIT needed

## License

MIT
