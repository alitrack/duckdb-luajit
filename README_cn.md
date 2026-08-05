# luajit — DuckDB LuaJIT UDF 扩展 v0.30

> 英文版：README.md（English version: [README.md](README.md)）

自包含的 DuckDB 扩展：Lua 表达式、JIT 编译 UDF、嵌套类型桥，基于 LuaJIT。约 1MB，MIT 协议。

> **v0.30 变更**：`_duckdb_query` 结果桥整数列按真实物理宽度读（INTEGER/SMALLINT/TINYINT/无符号族——此前全按 int64 读，非 BIGINT 列读出垃圾值，如 5 → 2147483648005）；存储过程式回查配方安全。

## 快速开始

```sql
LOAD 'luajit';
-- 先注册 UDF（compile 模式），再用注册名调用。
-- 标量 UDF 的第一个参数是「注册名」，不是 Lua 源码。
SELECT * FROM luajit_module(mode := 'compile', sql_name := 'x2',
                            source := 'return function(x) return x * 2 end');
SELECT luajit_i('x2', 21) AS r;          -- 42
SELECT * FROM luajit_module(mode := 'compile', sql_name := 'hello',
                            source := 'return function(x) return "hello " .. x end');
SELECT luajit_s('hello', 'world');        -- hello world
-- 表函数：Lua 源码 → 多行
SELECT * FROM luajit_table('return {"a", "b", "c"}');
```

`luajit_module(mode:='compile', sql_name:='NAME', source:='...')` 注册 UDF；
`quick_compile` 额外自动探测返回类型并创建 SQL 宏。

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
| `luajit_i/f/s/b` | 标量 UDF（int/float/varchar/boolean 参数） |
| `luajit_l` | LIST 输入（任意子类型） |
| `luajit_vs/vi` | 批量（chunk-batched）VARCHAR/BIGINT UDF |
| `luajit_agg` | 聚合 UDF（BIGINT/DOUBLE） |
| `luajit_table` | 表函数（table 行 / coroutine.wrap 流式 generator） |
| `luajit_module` | install（从 duckdb-luajit-libs 拉库）、list_remote、quick_compile（注册 UDF）、last_error 等控制面 |

## 远程库（install / list_remote）

[duckdb-luajit-libs](https://github.com/alitrack/duckdb-luajit-libs) 仓库的库可以
一行 SQL 安装——拉取、缓存到 `~/.duckdb/luajit-libs/`、注册（UDF 或模块表）：

```sql
LOAD 'luajit';
SELECT * FROM luajit_module(mode := 'list_remote');   -- 看有哪些库
SELECT * FROM luajit_module(mode := 'install', sql_name := 'export');  -- 一行安装
-- export 是标量 UDF：注册宏后即可 SQL 层调用
SELECT * FROM luajit_module(mode := 'quick_compile', sql_name := 'export',
                            source := (SELECT content FROM read_text('/home/USER/.duckdb/luajit-libs/export.lua')));
-- 或直接经 luajit_s / luajit_table 调用：
SELECT luajit_s('export', {query: 'SELECT 1', file: '/tmp/out.parquet'});
SELECT * FROM luajit_table('dirscan', list := '/path/to/files');
```

离线：库缓存后 `install` 和 `list_remote` 无需网络（先查本地缓存，INDEX 也会缓存）。

## 安全

trusted 沙箱模式移除 `io/os/ffi/package/require/load*`（不可触文件系统/网络/系统调用）；
普通模式保留全部 Lua 能力（文件、FFI、`_duckdb_query` 回查）。默认非 trusted。

## 版本历史

- **v0.30**: `_duckdb_query` 结果桥整数列按真实物理宽度读（INTEGER/SMALLINT/TINYINT/无符号族——此前全按 int64 读，非 BIGINT 列读出垃圾值，如 5 → 2147483648005）；存储过程式回查配方安全
- **v0.29**: `luajit_table` 表函数参数——list（路径列表，替代 io.popen 兜底）+ mode='blob'（BLOB 返回列，NUL 安全）；generator 写入改长度感知
- **v0.28**: BLOB 桥——`luajit_blob` 标量函数（Lua 字符串 ↔ BLOB，NUL 安全）+ BLOB 参数支持（fjs 长度感知写入）
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
