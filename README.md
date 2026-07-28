# luajit — DuckDB LuaJIT UDF Extension

Self-contained DuckDB extension for Lua expressions and UDFs via LuaJIT.

```sql
INSTALL luajit FROM community;
LOAD luajit;

-- Inline eval
SELECT luajit('return 1 + 2');                     -- '3'
SELECT luajit('return _VERSION');                   -- 'Lua 5.1'

-- Register Lua functions as named globals
SELECT ok FROM luajit_module(
    mode := 'compile',
    source := 'return function(a, b) return tostring(tonumber(a) + tonumber(b)) end',
    sql_name := 'my_add'
);

-- Call via luajit()
SELECT luajit('return my_add(3, 4)');               -- '7'

-- Multiple UDFs with correct per-function dispatch
SELECT ok FROM luajit_module(
    mode := 'compile',
    source := 'return function(s) return string.upper(s) end',
    sql_name := 'my_up'
);
SELECT luajit('return my_add(10, 20)'),             -- '30'
       luajit('return my_up("hello")');              -- 'HELLO'

-- Use with table columns
SELECT v, luajit('return my_add(' || v || ', 10)') FROM my_table;

-- Create a SQL macro for ergonomic access
CREATE MACRO add(a, b) AS luajit('return my_add(' || a || ',' || b || ')');
SELECT add(3, 4);                                    -- '7'
```

## Design

- **LuaJIT** (MIT): trace-based JIT, compiles hot paths to machine code
- **Self-contained**: ~700KB, no external Lua dependency
- **Cross-platform**: Linux/macOS/Windows (WASM pending)
- **UDFs as Lua globals**: simple, no per-function dispatch overhead

## API

| Function | Type | Description |
|----------|------|-------------|
| `luajit(expr)` | scalar → VARCHAR | Evaluate any Lua expression |
| `luajit_module(...)` | table function | Compile Lua function, store as named global |
| `luajit_module(mode:='info')` | table function | Extension version info |

## Build

```sh
git clone https://github.com/alitrack/luajit.git
cd luajit
./bootstrap.sh        # clones extension-ci-tools + LuaJIT
make release
make test_release
```

## License

MIT
