# luajit — DuckDB LuaJIT UDF Extension  v0.30

Self-contained DuckDB extension for Lua expressions, JIT-compiled UDFs, and nested type bridges via LuaJIT. ~1MB, MIT licensed.

> **v0.30 changes**: `_duckdb_query` result bridge now reads integer columns at their
> true physical width — INTEGER/SMALLINT/TINYINT and unsigned variants were previously
> all read as int64 (garbage values for non-BIGINT columns, e.g. `5` → `2147483648005`).
> This makes the SQL callback bridge (stored-procedure style recipes) safe for
> arbitrary integer columns.

中文版说明见 [README_cn.md](README_cn.md) / Chinese version: [README_cn.md](README_cn.md)

## Quick Start

```sql
LOAD 'luajit';
-- Anonymous function: pass Lua source directly — use-and-discard, no registration.
-- Body style: 'return <expr>' (x binds to the first argument):
SELECT luajit_i('return x * 2', 21) AS r;                    -- 42
SELECT luajit_s('return "hello " .. x', 'world');            -- hello world
-- Expression style (multi-arg):
SELECT luajit_i('function(a, b) return a + b end', 20, 22);  -- 42
-- Registered UDF: compile once, reuse by name (hot paths — compiled once per
-- session, not per chunk):
SELECT * FROM luajit_module(mode := 'compile', sql_name := 'x2',
                            source := 'return function(x) return x * 2 end');
SELECT luajit_i('x2', 21) AS r;                              -- 42
-- Table function: Lua source → rows
SELECT * FROM luajit_table('return {"a", "b", "c"}');
```

`luajit_module(mode:='compile', sql_name:='NAME', source:='...')` registers the
UDF; `quick_compile` additionally auto-probes return type and creates a SQL macro.
Anonymous source is compiled per chunk (use-and-discard); registered names are
reused (compile once per session) — prefer registration for hot paths.

## Lua Libraries (libs) — Load Functions/Table Functions with One SQL

Community extensions cover the mainstream formats (parquet/csv/iceberg); **long-tail
formats** (DICOM/EXIF/PDF/private APIs) are covered by Lua libraries — pull them
from [duckdb-luajit-libs](https://github.com/alitrack/duckdb-luajit-libs) with one
SQL statement, no compilation, no INSTALL:

```sql
LOAD 'luajit';
SET VARIABLE src = (SELECT content FROM read_text(
  'https://raw.githubusercontent.com/alitrack/duckdb-luajit-libs/main/libs/datasource/dicom.lua'));
-- Table function: scan directory, parse DICOM (1000 files ~40ms)
SELECT count(*) FROM luajit_table(getvariable('src'));
```

The libs repo is organized into categories: `datasource` (read files/directories) /
`parser` (JSON etc.) / `udf` / `network` / `ffi`; every library carries metadata in
its header (`@category/@desc/@requires`), with a ROADMAP (Phase 0-4) and contribution
guidelines.

## Function Overview

| Function | Purpose |
|---|---|
| `luajit_i/f/s/b` | Scalar UDFs (int/float/varchar/boolean params) |
| `luajit_l` | LIST input (any element type) |
| `luajit_vs/vi` | Batch (chunk-batched) VARCHAR/BIGINT UDFs |
| `luajit_agg` | Aggregate UDFs (BIGINT/DOUBLE) |
| `luajit_table` | Table function (table rows / coroutine.wrap streaming generator) |
| `luajit_module` | install (fetch lib from duckdb-luajit-libs), list_remote, quick_compile (register UDFs), last_error, and other control surface |

## Remote libs (install / list_remote)

Libraries from the [duckdb-luajit-libs](https://github.com/alitrack/duckdb-luajit-libs)
repo can be installed with one SQL call — fetched, cached to
`~/.duckdb/luajit-libs/`, and registered (UDF or module table):

```sql
LOAD 'luajit';
SELECT * FROM luajit_module(mode := 'list_remote');   -- what's available
SELECT * FROM luajit_module(mode := 'install', sql_name := 'export');  -- one-line install
-- export is a scalar UDF: register a macro for SQL-level calls
SELECT * FROM luajit_module(mode := 'quick_compile', sql_name := 'export',
                            source := (SELECT content FROM read_text('/home/USER/.duckdb/luajit-libs/export.lua')));
-- or call directly via luajit_s / luajit_table:
SELECT luajit_s('export', {query: 'SELECT 1', file: '/tmp/out.parquet'});
SELECT * FROM luajit_table('dirscan', list := '/path/to/files');
```

Offline: once a lib is cached, `install` and `list_remote` work without network
(local cache is checked first; INDEX is cached too).

## Security

trusted sandbox mode removes `io/os/ffi/package/require/load*` (no filesystem /
network / syscall access); normal mode keeps the full Lua capability set (files,
FFI, `_duckdb_query` callback). Default: not trusted.

## Changelog

- **v0.30**: `_duckdb_query` result bridge reads integer columns at their true
  physical width (INTEGER/SMALLINT/TINYINT and unsigned — previously read as int64,
  garbage values for non-BIGINT columns, e.g. 5 → 2147483648005); stored-procedure
  style callback recipes are now safe
- **v0.29**: `luajit_table` named parameters — list (path list, replaces io.popen
  fallback) + mode='blob' (BLOB return column, NUL-safe); generator writes are
  length-aware
- **v0.28**: BLOB bridge — `luajit_blob` scalar function (Lua string ↔ BLOB,
  NUL-safe) + BLOB parameter support (fjs length-aware writes)
- **v0.27**: generator shared non-TLS state fix (cross-thread registry mismatch);
  aligned with v1.5.5
- **v0.24**: `luajit_table` serial execution (`set_max_threads(1)`); CI matrix
  `skip_tests: true` (macOS arm64/Windows behavior differences), new
  `sqllogictest-linux` job
- **v0.23**: quick_compile auto-detects UDF style (scalar→`luajit_s/i/f/b`,
  batch→`luajit_vs`); `luajit_s` accepts arbitrary scalar params
- **v0.22**: quick_compile macros execute DDL directly; VARCHAR-returning UDFs map
  to `luajit_vs`; `luajit_l` accepts `LIST(ANY)`; luajit_table materialize fix;
  SQLLogicTests enabled
- **v0.21**: STRUCT bridge (DECIMAL any width / true element widths / NULL children /
  nested recursion); LIST bridge width-aware reads + DECIMAL elements; DuckDB
  decimal physical storage bands (≤4 int16 / ≤9 int32 / ≤18 int64 / else int128)
- **v0.20**: `luajit_agg` exact BIGINT returns; trusted sandbox; `_duckdb_query`
  result bridge; DATE/TIMESTAMP/DECIMAL/HUGEINT bridges; streaming generator;
  chunk-batched UDFs; LIST bridge NULL/BOOLEAN; MSVC fixes
- **v0.19**: GC64 (2GB memory wall removed); UDF resolution fixes
- **v0.18 and earlier**: type bridge evolution (see git log)

## Resources

- [duckdb-luajit-libs](https://github.com/alitrack/duckdb-luajit-libs) — Lua library
  repo (categories + install protocol + ROADMAP)
- WeChat articles (Chinese): type bridges / signed-API data sources / DICOM
  (83 lines) / directory scan (110 lines)
- Community extension PR: duckdb/community-extensions#2428
