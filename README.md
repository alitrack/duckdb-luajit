# luajit — DuckDB LuaJIT UDF Extension

DuckDB community extension for in-process JIT-compiled Lua UDFs via LuaJIT.

**Self-contained. Cross-platform. Sandbox-aware.**

## Quick Start

```sql
INSTALL luajit FROM community;
LOAD luajit;

-- Compile and register a Lua function as a SQL UDF
SELECT ok, mode, code FROM luajit_module(
    mode := 'quick_compile',
    source := 'return function(a, b) return a + b end',
    sql_name := 'lua_add',
    return_type := 'i64',
    arg_types := ['i64', 'i64']
);

-- Call the registered UDF
SELECT lua_add(1, 2) AS result;
```

## Design Principles

- **LuaJIT FFI** for zero-overhead DuckDB C API interop — no C wrapper codegen needed
- **Self-contained** — LuaJIT runtime baked into the extension binary
- **Cross-platform** — Linux (amd64/arm64), macOS (amd64/arm64), Windows, WASM
- **Sandbox-friendly** — Lua's built-in environment isolation beats raw C
- **Type bridge** — full DuckDB type system mapped to Lua/LuaJIT FFI types

## Platform Support

| Platform | Status |
|----------|--------|
| linux_amd64 | ✅ |
| linux_arm64 | ✅ |
| osx_amd64 | ✅ |
| osx_arm64 | ✅ |
| windows_amd64 | ✅ |
| wasm_mvp | ✅ |
| wasm_eh | ✅ |
| wasm_threads | ✅ |

## Architecture

```
SQL: luajit_module(...)
     ↓
luajit_module.c (control plane)
     ├── session staging (source, includes, bind management)
     ├── type signature parsing (SQL types → LuaJIT FFI types)
     ├── LuaJIT FFI bridge (direct DuckDB C API calls, no C glue code)
     ├── function registration via duckdb extension API
     └── luajit_execute_udf (runtime vector marshaling)
```

## Comparison

| Feature | DuckTinyCC (C/TCC) | luajit (LuaJIT) |
|---------|-------------------|-----------------|
| Language | C | Lua |
| Compilation | libtcc → machine code | LuaJIT trace → asm |
| Windows | ❌ | ✅ |
| WASM | ❌ | ✅ |
| Sandbox | ❌ (bare metal) | ✅ (env isolation) |
| FFI bridge | Codegen wrapper C | Direct ffi.C.* calls |
| Type bridge LOC | ~3000 | ~500 (ffi eliminates most) |

## Build

```sh
make configure
make debug
make release
make test_debug
make test_release
```

## License

MIT
