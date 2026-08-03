# luajit — DuckDB LuaJIT UDF Extension  v0.27

Self-contained DuckDB extension for Lua expressions, JIT-compiled UDFs, and nested type bridges via LuaJIT. ~700KB, MIT licensed.

> **v0.27 changes**: `luajit_table` generator mode fixed — the coroutine.wrap
> iterator was registered in the init thread's TLS lua_State but resumed from
> other threads' TLS states (cross-state registry lookup → nil → intermittent
> 0 rows). Fix: generator uses a shared non-TLS state (`lua_xmove` the
> iterator there); all workers resume the same coroutine under the global
> lock. Verified 100/100 independent runs + full SQLLogicTests. Built/tested
> against DuckDB v1.5.5.

## 快速开始

```sql
LOAD 'luajit';
-- JIT 编译的 Lua 表达式/标量 UDF
SELECT luajit_i('return x * 2', 21) AS r;          -- 42
SELECT luajit_s('return "hello " .. x', 'world');  -- hello world
-- 表函数：Lua 源码 → 多行
SELECT * FROM luajit_table('return {"a", "b", "c"}');
```

## Lua 库模式（libs）——一条 SQL 加载函数/表函数

社区扩展覆盖主流格式（parquet/csv/iceberg）；**长尾格式**（DICOM/EXIF/PDF/私有 API）
用 Lua 库补——从 [duckdb-luajit-libs](https://github.com/alitrack/duckdb-luajit-libs)
一条 SQL 拉取即用，无需编译、无需 INSTALL：

```sql
LOAD 'luajit';
SET VARIABLE src = (SELECT content FROM read_text(
  'https://raw.githubusercontent.com/alitrack/duckdb-luajit-libs/main/libs/datasource/dicom.lua'));
-- 表函数：扫目录解析 DICOM（1000 文件 ~40ms）
SELECT count(*) FROM luajit_table(getvariable('src'));
```

libs 仓库分类：`datasource`（读文件/目录）/ `parser`（JSON 等）/ `udf` / `network` / `ffi`，
每库头部带元数据（`@category/@desc/@requires`），有 ROADMAP（Phase 0-4）与贡献规范。

## 函数总览

| 函数 | 用途 |
|---|---|
| `luajit_i/f/s/b` | 标量 UDF（int/float/varchar/blob 参数） |
| `luajit_l` | LIST 输入（任意子类型） |
| `luajit_vs/vi` | 批量（chunk-batched）VARCHAR/BIGINT UDF |
| `luajit_agg` | 聚合 UDF（BIGINT/DOUBLE） |
| `luajit_table` | 表函数（table 行 / coroutine.wrap generator 流式） |
| `luajit_module` | quick_compile（注册 UDF）、last_error 等控制面 |

## 安全

trusted 沙箱模式移除 `io/os/ffi/package/require/load*`（不可触文件系统/网络/系统调用）；
普通模式保留全部 Lua 能力（文件、FFI、`_duckdb_query` 回查）。默认非 trusted。

## 版本历史

- **v0.27**: generator 共享非 TLS state 修复（跨线程 registry 错位）；v1.5.5 对齐
- **v0.24**: `luajit_table` 串行执行（`set_max_threads(1)`）；CI 构建矩阵 `skip_tests: true`（macOS arm64/Windows 行为差异），新增 `sqllogictest-linux` job
- **v0.23**: quick_compile 自动识别 UDF 风格（标量→`luajit_s/i/f/b`、批量→`luajit_vs`）；`luajit_s` 接受任意标量参数
- **v0.22**: quick_compile 宏直接执行 DDL；VARCHAR 返回 UDF 映射 `luajit_vs`；`luajit_l` 接受 `LIST(ANY)`；luajit_table materialize 修复；SQLLogicTests 启用
- **v0.21**: STRUCT 桥（DECIMAL 任意宽度/真元素宽度读/NULL 子项/嵌套递归）；LIST 桥按宽度读 + DECIMAL 元素；DuckDB decimal 物理存储按宽度分带（≤4 int16 / ≤9 int32 / ≤18 int64 / 否则 int128）
- **v0.20**: `luajit_agg` BIGINT 精确返回；trusted 沙箱；`_duckdb_query` 结果桥；DATE/TIMESTAMP/DECIMAL/HUGEINT 桥；流式 generator；chunk 批量 UDF；LIST 桥 NULL/BOOLEAN；MSVC 修复
- **v0.19**: GC64（2GB 内存墙移除）；UDF 解析修复
- **v0.18 及更早**: 类型桥演化（见 git log）

## 相关资源

- [duckdb-luajit-libs](https://github.com/alitrack/duckdb-luajit-libs) — Lua 库仓库（分类 + 安装协议 + ROADMAP）
- 系列文章：类型补全 / 签名 API 数据源 / DICOM（83 行）/ 目录扫描（110 行）——公众号「测试号」
- 社区扩展 PR: duckdb/community-extensions#2428（CI 全绿，待合并）
