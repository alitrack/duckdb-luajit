---
name: duckdb-luajit-cn-data
description: >
  China-domain data wrangling inside DuckDB: 数电发票 OFD e-invoice parsing,
  通达信 TDX stock quote files, ID-card / social-credit-code checksum
  validation, GBK/encoding conversion, and pinyin conversion. Use when the
  query touches Chinese financial or identity data formats that international
  extensions do not cover.
version: 0.33.0
---

# duckdb-luajit: China-domain data sources

These libraries fill the gap international DuckDB extensions leave open:
China-specific formats, all installable with one SQL.

## 数电发票 OFD (China fully-digitalized e-invoice)

```sql
SELECT * FROM luajit_module(mode := 'install', sql_name := 'inv_ofd');
-- table function: metadata (invoice number / amount / tax IDs / issue date)
-- + layout text lines per page
SELECT * FROM luajit_table('inv_ofd_scan', list := '/path/to/invoices');
```

## 通达信 TDX stock quotes

`.day` (daily) / `.lc1` (1-min) / `.lc5` (5-min): 32-byte little-endian
records parsed straight to rows.

```sql
SELECT * FROM luajit_module(mode := 'install', sql_name := 'tdx');
SELECT * FROM luajit_table('tdx_read', list := '/data/sh000001.day');
```

## China checksum validation (cncheck)

```sql
SELECT * FROM luajit_module(mode := 'install', sql_name := 'cncheck');
-- resident ID card numbers, unified social credit codes, and more —
-- checksum algorithms in pure Lua
SELECT luajit_s('cncheck_id', id_col) FROM customers;
```

## Encoding conversion (iconv family)

`enc_detect` (encoding detection) / `convert` (iconv transcode,
`//IGNORE` strips illegal bytes) — for GBK/GB2312/Big5 legacy exports:

```sql
SELECT * FROM luajit_module(mode := 'install', sql_name := 'iconv');
SELECT luajit_s('iconv_convert', gbk_text, 'GBK', 'UTF-8');
```

## Pinyin

```sql
SELECT * FROM luajit_module(mode := 'install', sql_name := 'pinyin');
SELECT luajit_s('pinyin', '中文');          -- zhōngwén (vendored pypinyin dict)
```

Combine with DuckDB FTS after jieba-style segmentation, or use for
sort/match keys on Chinese names.
