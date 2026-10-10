---
name: duckdb-luajit-libs
description: >
  Install ready-made Lua libraries into DuckDB with one SQL statement from
  duckdb-luajit-libs: parsers (JSON/YAML/XML/TOML/HTML/EPUB/RSS), data
  sources (DICOM, EXIF, OFD invoices, TDX quotes), ETL (incremental, SCD2),
  privacy (differential privacy, PII masking, k-anonymity), network, FFI
  kernels (HiGHS, LAPACK, curl). Use when the task needs a long-tail format
  or capability DuckDB has no extension for.
version: 0.33.0
---

# duckdb-luajit: one-SQL libraries (duckdb-luajit-libs)

50+ libraries, no compilation, no extra INSTALL. Cached to
`~/.duckdb/luajit-libs/` after first fetch; sha256-verified against
INDEX.v2 (fail-closed, verified on download AND cache hit).

## Discovery & install

```sql
LOAD luajit;
SELECT * FROM luajit_module(mode := 'list_remote');           -- what's available
SELECT * FROM luajit_module(mode := 'install', sql_name := 'json');
-- export-style libs register a scalar UDF; wrap with a macro:
SELECT * FROM luajit_module(mode := 'quick_compile', sql_name := 'export',
    source := (SELECT content FROM read_text(
      '/home/USER/.duckdb/luajit-libs/export.lua')));
SELECT luajit_s('export', {query: 'SELECT 1', file: '/tmp/out.parquet'});
```

Offline: once cached, `install` and `list_remote` work without network.

## Library map (pick by task)

| Task | lib (category/name) |
|---|---|
| YAML / TOML / XML / HTML / EPUB / RSS / JSONPath / JSON Patch | parser/* (pure Lua) |
| DICOM medical images (dir scan, 1000 files ~40ms) | datasource/dicom |
| EXIF + PDF info + file-type magic on a directory | datasource/dirscan |
| 数电发票 OFD (China e-invoice metadata + layout lines) | datasource/inv_ofd |
| 通达信 .day/.lc1/.lc5 stock quotes | datasource/tdx |
| Incremental watermark / SCD2 dimensions | etl/incremental, etl/scd2 |
| Differential privacy / PII masking / k-anonymity | privacy/privacy |
| LP/QP/MILP solving (HiGHS kernel via FFI) | optimize/highs |
| LAPACK/OpenBLAS linear algebra | linalg/linalg |
| HTTP client without forking curl | udf/curl_ffi |
| QR codes, pinyin, base64, CRC32, fuzzy distance, UUID | udf/* |
| China checksum IDs (身份证/统一社会信用代码 etc.) | udf/cncheck |
| Random from 40+ distributions | stats/rng |
| Record linkage (blocking → scoring → components) | entity/entity |
| PSI drift / KL / chi-square data monitoring | quality/psi |
| Audit-log hash chain, Quack RBAC | security/audit_chain, security/rbac |

Many libs are table functions — call through `luajit_table`:

```sql
SELECT count(*) FROM luajit_table(getvariable('src'));  -- src = lib source text
```

## Maturity metadata

Each lib header carries `@category/@desc/@requires/@maturity`
(tested / audited / experimental) — check before production use.
FFI-based libs (highs, linalg, curl_ffi, classifier) require the native
shared library; see the repo's SBOM and FFI docs.
