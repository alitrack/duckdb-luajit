# Lua 能力清单 — duckdb-luajit 核武器手册

> 配套文档：`README.md`（API 参考）。本文回答一个问题：**在 SQL 里写 Lua，到底能干什么？**

## 一、语言核心（UDF 里能用的完整 Lua）

| 能力 | 说明 |
|---|---|
| 语法 | 完整 Lua 5.1 + LuaJIT 扩展（bit、jit、ffi、string.buffer、cdata） |
| 函数式 | 一等函数、闭包、多返回值、`select`/`unpack`、尾调用 |
| 数据结构 | table（数组+哈希一体）、metatable 元编程、coroutine 协程 |
| 数值 | `double`（±2^53 内整数精确）；`42LL` → int64 cdata；`bit.*` 32 位位运算 |
| 字符串 | 不可变 + interned；`string.*` 全库；`string.buffer` 高效拼接 |
| 标准库 | base / string / table / math / io / os / bit / jit / ffi / debug / coroutine / package |
| 错误处理 | `pcall` / `xpcall` / `error` |

## 二、核武器清单（按威力排序）

### 🚀 N1. FFI — 直接调任意 C 函数、读写 C 内存
SQL 里写 Lua，Lua 里直接调 C（**注意：LuaJIT 的 ffi 不是全局变量，必须 `require('ffi')`**）：
```lua
local ffi = require('ffi')
ffi.cdef[[ double erf(double); ]]        -- 声明任意 C 函数
local libm = ffi.load('m')               -- 加载 libm
-- 之后：erf、lgamma、j0... 标准 Lua 数学库没有的都在这
```
- **能力**：加载任何共享库（libm / zlib / 自定义 .so），定义结构体、数组、指针，零拷贝访问
- **危险**：无沙箱——能读文件、执行命令、崩进程。生产环境慎开
- **配套**：`luajit_l`/`luajit_s`/`luajit_map` 把 DuckDB 嵌套类型桥成 Lua table 后，可直接喂给 FFI

### 🚀 N2. JIT — 热循环编译为机器码
- 批量模式（`luajit_v`）：1 次 Lua 调用处理整个 chunk，Lua 侧循环被 trace 编译 → **3.6× 原生 SQL**（单线程）
- 行模式（`luajit_i/f/b/m`）：每行 1 次 Lua 调用，仍比 Python UDF 快 ~2500×
- 数值计算（纯 Lua 循环 + double）接近 C 速度

### 🚀 N3. 聚合核武器 — `luajit_agg`
SQL 没有的聚合，Lua 任意写：
```sql
-- 中位数 / 分位数 / 自定义指标——只要 Lua 能写的聚合都行
SELECT luajit_agg('mymedian', price) FROM trades;
SELECT g, luajit_agg('mystd', v) FROM data GROUP BY g;
```
- 收全组值 → 一次 Lua 调用（finalize 时），避开逐行 Lua 开销
- GROUP BY 支持（≤256 组）

### 🚀 N4. 表生成 — `luajit_table`
SQL 里生成任意行集：
```sql
SELECT * FROM luajit_table('return function() 
  local t = {}
  for i = 1, 100 do t[i] = {i, 'row-'..i} end
  return t end');
```
- 模拟数据、测试数据、数列、序列生成

### 🚀 N5. 嵌套类型桥 — LIST / STRUCT / MAP
```sql
-- LIST → Lua 数组（1-indexed），STRUCT → 命名键表，MAP → k-v 表
SELECT luajit_l('top2', [3,1,4,2]);        -- [4.0, 3.0]
SELECT luajit_s('fmt', {x:3, y:4});        -- 'x=3 y=4'
SELECT luajit_map('fmt', map{'a':1});      -- 'a=1'
```
- 复杂 JSON/嵌套逻辑直接在 Lua 侧写，不用 SQL 递归 CTE

### 🚀 N6. Lua→SQL 回调 — `_duckdb_call`
```sql
SELECT luajit('return _duckdb_call("CREATE TABLE log(ts TIMESTAMP DEFAULT NOW())")');
```
- Lua 里执行任意 SQL：动态建表、写日志、查表回填
- 元编程：根据数据决定 schema

### 🚀 N7. UDF 持久化 — save / load
```sql
SELECT message FROM luajit_module(mode := 'save', source := 'my_udfs.txt');
-- 重启后 LOAD 扩展自动恢复全部 UDF
```
- 把 Lua 逻辑当"存储过程"跨会话复用

## 三、能力边界（诚实清单）

| 边界 | 现状 | 状态 |
|---|---|---|
| 语言版本 | Lua 5.1 语法；**无** `goto`/`//`/`<<`/`_ENV`（5.2+/5.3 特性） | 不可变 |
| 数值精度 | `double` 全类型；整数 > 2^53 需 `LL` cdata；`bit.*` 仅 32 位 | 不可变 |
| 内存 | GC64 已开启 → 2GB 硬墙解除（v0.18+） | ✅ 已修 |
| 并发 | 全局锁串行；多线程 DuckDB 执行时 UDF 串行 | 多 state 池规划中 |
| 沙箱 | **无**。`os.execute`/`io`/`ffi` 全开 | 文档警告 |
| 包管理 | `require` 可用但路径受限（无 luarocks 生态） | 不可变 |
| 错误 | Lua 运行时错误 → 行置 NULL + `luajit_module(mode:='last_error')` 查询（v0.18+） | ✅ 已修 |
| 类型覆盖 | 标量: BIGINT/DOUBLE/BOOLEAN/VARCHAR；嵌套: LIST(BIGINT)/STRUCT/MAP 部分类型 | 扩展中 |

## 四、典型场景矩阵

| 场景 | 用哪个 | 示例 |
|---|---|---|
| 数值/统计计算 | `luajit_v` / `luajit_agg` | 波动率、分位数、自定义指标 |
| 字符串清洗/解析 | `luajit` / `luajit_m` | 正则式拆分、编码转换、格式化 |
| 特征工程 | `luajit_f` / `luajit_v` | 时间序列特征、归一化、衍生变量 |
| 嵌套数据逻辑 | `luajit_l/s/map` | JSON 提取、LIST 重排、MAP 聚合 |
| 数据生成 | `luajit_table` | 测试数据、模拟序列 |
| 动态 schema | `_duckdb_call` | 按数据建表、回写日志 |
| 复杂算法 | FFI + `luajit_v` | 调 C 库（libm、zlib、自定义） |

## 五、性能心法（写 UDF 前三分钟必读）

1. **优先批量**：`luajit_v`（chunk 级）>> `luajit_i/f/b/m`（行级）。JIT 在 Lua 循环内编译，跨 C 边界不优化
2. **NYI 黑名单**（用了掉回解释器 10–100×）：`pairs`/`next`（用 `ipairs`）、`string.gsub`（用 find+sub）、`unpack`、循环内建闭包、多变参 `...`
3. **数值**：`double` 优先、`int32_t` 优先于 `uint32_t`、避免 `float`；少无偏分支（用 `math.min/max`、`bit.*`）
4. **局部化**：`local sin = math.sin`；函数全部 `local`
5. **字符串**：热循环里别 `..` 拼接（interned 不释放）→ `table.concat` / `string.buffer`
6. **别手写 CSE**：`a[i][j] = a[i][j] * a[i][j+1]` 放心写，JIT 比你聪明
7. **LIST 桥的 NULL 元素**：DuckDB 的 `[1, NULL, 3]` 到 Lua 是 `{1, nil, 3}`——Lua 的 `#` 和 `ipairs` 遇到中间 nil 会**截断**（`#{1,nil,3}` = 1）。遍历带 NULL 的列表请用 `for i=1, table.maxn(t)` 或显式 `t[i] ~= nil` 判断，或用 `select('#', ...)` 风格计数
