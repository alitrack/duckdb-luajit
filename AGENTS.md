# AGENTS.md — duckdb-luajit

DuckDB 进程内 LuaJIT UDF 扩展：SQL 里直接写 Lua（JIT 编译到机器码），并行执行（per-thread lua_State 无全局锁），trusted 沙箱，聚合/表函数/嵌套类型桥/内嵌 Fennel。C 实现，产物 `luajit.duckdb_extension`。**对外一律称全名 duckdb-luajit，禁只写「lua」**（另有 isaacbrodsky/duckdb-lua 竞品易混淆）。

## Build & Test
```bash
make configure          # 首次：venv + platform + extension_version
make debug              # debug 扩展
make release            # release 扩展（默认 all 目标）
make test               # sqllogictest（默认 SKIP_TESTS=1 跳过）
make test_release SKIP_TESTS=0    # 本地跑 sqllogictest（luajit_bridge.test）
duckdb -unsigned -c "LOAD 'build/release/luajit.duckdb_extension'; ..."
```
- 依赖 DuckDB C API 版本：`TARGET_DUCKDB_VERSION=v1.2.0`（Makefile）；**开发环境对齐 DuckDB 1.5.5**（本地 CLI 在 `~/.duckdb/cli/1.5.5`）
- 未签名本地扩展必须 `-unsigned` 加载，否则报 signature invalid
- 改 .c 后 `make release` 可能不重编 → `touch src/<file>.c` 强制重编，确认出现 "Building C object" 行

## Architecture
- `src/luajit_entry.c` — 扩展入口、DuckDB API 绑定
- `src/luajit_module.c` — 核心：UDF 注册/执行、类型桥、luajit_module 14 modes（compile/quick_compile/fennel/trusted/inspect/macro/list/drop/reset/save/load/last_error/info）、per-db 注册表
- `src/embedded/` — 内嵌组件（LuaJIT 运行时、Fennel 编译器）
- `src/include/` — 内部头文件
- `duckdb_capi/`、`extension-ci-tools/` — C API 头与构建工具链（非 submodule）
- `docs/` — lua-capabilities.md、syntax-layers.md
- **UDF 持久化 = 数据库文件内 `luajit_udf_registry` 表**（绑 db 不绑 HOME；换 .db 不加载）

## Key Patterns
- 16 执行器：标量 `luajit_i/f/b`（匿名源码直传或注册名）、批量 `luajit_v/vi/vs`（chunk 批量 ~1.9×）、类型桥 `luajit_l/s/m`（LIST/STRUCT/MAP）、`luajit_agg`（GROUP BY 聚合）、`luajit_table`（流式表函数）、`luajit_module`（管理）
- **匿名源码 = 逐行标量语义；注册名 = 批量收 table 契约**。标量 UDF 全 NULL → `mode := 'last_error'` 查错 → "table value" = 标量函数走了批量执行器 → 换 luajit_s/i/f/b
- 标量并发 = per-thread lua_State（TLS，无全局锁）；表函数串行（set_max_threads(1)）+ 共享非 TLS state（generator 跨线程）
- trusted 沙箱移除 io/ffi/package/require/load*/debug（普通模式才有 `_duckdb_call`/`_duckdb_query` 回查桥）
- 类型桥：DECIMAL 按物理宽度（int16/32/64/hugeint128）、STRUCT 单层、LIST 只收 BIGINT[]

## Risk Gates
- AUTO-APPROVED: 单个执行器内 Rust/C 改动、sqllogictest 用例、docs/、README
- REQUIRES APPROVAL: luajit_module.c 注册表/持久化逻辑、类型桥行为变更、trusted 沙箱边界、description.yml version+repo.ref bump（发版流程）
- BOUNDARY: 社区扩展 PR（community-extensions fork）、release tag 推送、extension-ci-tools 版本、excluded_platforms

## Conventions
- **产物名 `luajit.duckdb_extension`**；repo 是 duckdb-luajit；libs 库仓库是 duckdb-luajit-libs（`/mnt/d/wsl2/duckdb-luajit-libs`，INDEX 只收可编译 Lua chunk 的 .lua，FFI 源码资产放 `libs/ffi/` 不进 INDEX）
- 发版：功能版 = description.yml version + repo.ref 同步 + git tag；hotfix 不打自家 description.yml（社区 PR 分支的 version/ref 始终指向最新可发布 tag）
- **DuckDB vector 字符串必须用 length 字段限定，禁 strlen 语义**（string_t 无 NUL 结尾，本地绿 CI 红根因）
- 输出 vector 未全量写入必须显式 invalidate（NULL-free fast path 不认"留空"）
- 分配器匹配：duckdb_malloc ↔ duckdb_free，strdup/malloc ↔ free，混用 UB
- 多语句 `duckdb -c` 批跑一条失败可能整体空输出 → 逐条拆跑；CLI 验证输出用 `| cat` 勿 grep 表格线
- 行为验证纪律：修复后独立进程连跑 50-100 次确认 0 失败（样本小会误判）
- 发版/PR 前必须仔细 review（历史教训：destroy 未初始化 result、Windows 反斜杠路径、-Wformat-truncation）
