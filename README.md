# luajit — DuckDB LuaJIT UDF Extension

DuckDB community extension for in-process JIT-compiled Lua UDFs via LuaJIT.

**Self-contained. Cross-platform. Fast.**

```sql
INSTALL luajit FROM community;
LOAD luajit;

-- Inline evaluation
SELECT luajit('return 1 + 2');                    -- '3'
SELECT luajit('return _VERSION');                  -- 'Lua 5.1'

-- Register named UDFs
SELECT ok FROM luajit_module(
    mode := 'compile',
    source := 'return function(a, b) return tostring(tonumber(a) + tonumber(b)) end',
    sql_name := 'lua_add'
);
SELECT lua_add('3', '4');                          -- '7'

-- Use with table data
SELECT v, lua_mul10(v) FROM my_table;
```

## Design

- **LuaJIT** (MIT-licensed fork of Lua 5.1): trace-based JIT compiles hot paths to machine code
- **Self-contained**: LuaJIT runtime statically linked, ~700KB extension binary
- **Cross-platform**: Linux/macOS/Windows/WASM — LuaJIT is pure ANSI C

## API

| Function | Type | Description |
|----------|------|-------------|
| `luajit(expr)` | scalar → VARCHAR | Evaluate Lua expression inline |
| `luajit_module(...)` | table function | Compile and register named Lua UDFs |

## Build

```sh
git clone --recursive https://github.com/alitrack/luajit.git
cd luajit
# Clone LuaJIT into third_party/LuaJIT (from https://github.com/LuaJIT/LuaJIT)
make configure
make release
make test_release
```

## License

MIT
