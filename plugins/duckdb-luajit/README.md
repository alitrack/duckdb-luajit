# duckdb-luajit — Claude Code Plugin

Five skills that give your AI assistant verified, source-checked knowledge of
the [duckdb-luajit](https://github.com/alitrack/duckdb-luajit) DuckDB
extension's SQL API — function signatures, modes, security levels, and the
[duckdb-luajit-libs](https://github.com/alitrack/duckdb-luajit-libs) library
catalog. Instead of guessing at parameter names, the assistant writes correct
SQL from a plain-English request like "parse every OFD invoice in this
directory" or "register a slugify UDF and use it on the products table".

## Install

```text
/plugin marketplace add alitrack/duckdb-luajit
/plugin install duckdb-luajit@duckdb-luajit
```

For development against this repo: `claude --plugin-dir ./plugins/duckdb-luajit`.

## Skills

| Skill | Covers |
|---|---|
| `duckdb-luajit-udfs` | Decision tree, anonymous expressions, registered/quick_compile UDFs, aggregates, persistence |
| `duckdb-luajit-tables` | luajit_table rows/generators, type bridges (LIST/STRUCT/DECIMAL/BLOB), `_duckdb_query` |
| `duckdb-luajit-libs` | One-SQL library installs, catalog by task, maturity metadata |
| `duckdb-luajit-security` | full/restricted/sandbox levels, FFI allowlist, INDEX.v2 verification, deployment checklist |
| `duckdb-luajit-cn-data` | China-domain formats: OFD e-invoices, TDX quotes, cncheck, iconv/GBK, pinyin |

## Prerequisite

The skills assume the extension is installed:

```sql
INSTALL luajit FROM community;
LOAD luajit;
```

## Attribution

MIT, alitrack. Skills are maintained in
[`plugins/duckdb-luajit/skills/`](plugins/duckdb-luajit/skills/) and kept in
sync with the extension's README and docs at each release.
