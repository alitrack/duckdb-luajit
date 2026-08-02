# 语法层：Fennel / Teal / MoonScript — Lua 的"更好语法"，零引擎成本

> 配套文档：`lua-capabilities.md`（能力边界）、`README.md`（API 参考）。
> 本文回答一个问题：**嫌 Lua 语法别扭，但又不想换引擎——怎么办？**

结论先行：**引擎不变、语法升级**。Fennel / Teal / MoonScript 都是**编译到标准 Lua** 的
方言，编译产物可以直接喂给 duckdb-luajit 的 `luajit_module(mode:='compile')`——
完全复用现有 JIT 性能、批量通道、trusted 沙箱，运行时零额外开销。

## 为什么这条路成立（对比换引擎）

| 维度 | 换引擎（Wren 等） | 语法层（Fennel 等） |
|---|---|---|
| 性能 | 解释器档位，差 10-50× | **编译产物=普通 Lua**，JIT 照常 |
| 生态 | 空（数据场景要手写一切） | **继承全部 Lua 生态**（LPeg、lua-cjson、penlight…） |
| 维护 | 引擎依赖（0.x 5 年不发版 = 依赖炸弹） | 编译器只在开发期用，**运行时零依赖** |
| 发布 | 新插件：CI、三平台、签名全重来 | **零新插件**，现有 luajit.duckdb_extension 直接吃 |
| 团队 | 全员学新语言 | 语法糖，懂 Lua 即懂 |

## 一、Fennel（推荐）— Lisp 方言

**特点**：宏（编译期代码生成）、`match` 模式匹配、`where` 守卫、不可变局部、
`->`/`-?>` 管道。编译产物是**标准 Lua 5.1 兼容代码**（LuaJIT 完美支持）。

### 工具链（3 条命令）

```bash
# 1. 下载单文件编译器（纯 Lua，无依赖）
curl -L -o fennel https://fennel-lang.org/downloads/fennel-1.5.3
chmod +x fennel

# 2. 编译 .fnl → .lua（可用任意 Lua/LuaJIT 解释器跑 fennel）
luajit fennel --compile my_udf.fnl > my_udf.lua
```

### 工作流（实测验证过）

```fennel
;; my_udf.fnl — 模式匹配分类器
(fn classify [x]
  (match x
    0 "zero"
    (where n (> n 0)) "positive"
    _ "negative"))
```

```bash
luajit fennel --compile my_udf.fnl > classify.lua
# 产物是标准 Lua：match 展开成 if/else，宏编译期展开，运行时零开销
```

```sql
-- 产物直接喂给 duckdb-luajit（实测：compile 一次成功，结果正确）
SELECT message FROM luajit_module(
  mode:='compile',
  source:=$$
    return function(x)
      if (x == 0) then
        return "zero"
      else
        local and_1_ = (nil ~= x)
        if and_1_ then
          local n = x
          and_1_ = (n > 0)
        end
        if and_1_ then
          local n = x
          return "positive"
        else
          local _ = x
          return "negative"
        end
      end
    end
  $$,
  sql_name:='clsfy');

SELECT luajit('return clsfy(5)');   -- positive
SELECT luajit('return clsfy(-1)');  -- negative
SELECT luajit('return clsfy(0)');   -- zero
```

### 宏示例（Fennel 独有优势）

```fennel
;; 宏在编译期展开——运行时没有函数调用开销
(macro unless [cond body]
  `(if (not ,cond) ,body))

(fn safe-divide [a b]
  (unless (= b 0)
    (/ a b)))
```
编译产物里 `unless` 已被替换为 `if (not ...)`，JIT 全速执行。

## 二、Teal — 静态类型层

**特点**：Lua 的类型注解方言（`local x: number`、`record` 类型），编译期类型检查，
产物仍是标准 Lua。适合团队协作、大型 UDF 集合的类型安全。

```teal
-- my_udf.tl
local record Point
  x: number
  y: number
end

local function dist(a: Point, b: Point): number
  return math.sqrt((a.x-b.x)^2 + (a.y-b.y)^2)
end

return dist
```

```bash
# 安装 tl 编译器（纯 Lua）
# luarocks install tl 或 https://github.com/teal-language/tl/releases
tl gen my_udf.tl     # → my_udf.lua（标准 Lua）
```

## 三、MoonScript — 语法糖

**特点**：类语法干净（`class A extends B`）、缩进式代码。产物标准 Lua。

```moon
-- my_udf.moon
class Counter
  new: (start) => @n = start
  inc: => @n += 1

return (c) -> tostring(c\inc!)
```

```bash
# moonc 编译器（纯 Lua）
moonc my_udf.moon    # → my_udf.lua
```

## 四、注意事项

1. **编译产物检查**：喂给 compile 前先 `luajit fennel --compile x.fnl` 目测产物——
   出现 `require("fennel")` 或非 5.1 语法说明写错了方言（编译器选项 `--lua 5.1`）
2. **保持运行时零依赖**：产物不应 `require` 方言运行时库（Fennel/Teal/MoonScript
   的产物是自包含标准 Lua；若引入外部 Lua 库，用现有 `require` 通道 + 白名单）
3. **trusted 沙箱兼容**：产物是普通 Lua，沙箱规则不变（io/ffi/require 仍受 trusted 控制）
4. **批量/聚合照常**：`luajit_vi`/`luajit_vs`/`luajit_agg` 接受任何编译产物 UDF——
   语法层与性能通道正交
5. **命名规范**：编译产物 `local function classify(x)` + `return classify`——
   保持 `return function` 形状（compile 要求 UDF 源码整体是一个返回函数的表达式）

## 五、为什么不是换引擎（决策记录）

Wren × DuckDB 插件方案已评估并**否决**（2026-08-02）：
1. 性能硬伤——官方自认 ≈ LuaJIT 关 JIT 档位，开 JIT 的 duckdb-luajit 快一个量级以上
2. 生态真空——无库生态，数据场景的 JSON/日期/正则全要手写
3. 维护风险——0.4.0 停更 5 年，API 未冻结
4. 零先例——社区脚本引擎位已占满（lua/javascript/python/R），无 Wren

脚本引擎位已由 LuaJIT 占住；可读性诉求由语法层（本文）零成本满足。
