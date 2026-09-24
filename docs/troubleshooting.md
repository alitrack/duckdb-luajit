# Troubleshooting

Field-tested pitfalls, ordered by how often they bite. (Extracted from the
maintainer's debugging logs; most of these were real bugs at some point —
the extension now guards many of them, but the patterns still matter.)

## "My UDF returns NULL"

Silent-failure by design (resolve/compile errors return NULL, they don't
throw). **Always ask for the reason**:

```sql
SELECT message FROM luajit_module(mode := 'last_error');
```

Common causes:

| last_error says | Meaning | Fix |
|---|---|---|
| `UDF 'x' is not registered...` | `:memory:` db — registration is per-session | re-run `install`/`quick_compile` after connect |
| `attempt to perform arithmetic on local 'a' (a table value)` | return-type probe mis-fire (batch vs scalar) | check your function is `return function(a,b) ... end` form |
| `must return a function` | source is statements, not a factory | wrap: `return function(x) ... end` |
| `not found in INDEX` | lib not in INDEX.v2 (local dev) | `cp libs/.../x.lua ~/.duckdb/luajit-libs/` then install |

## Constant-folding traps in benchmarks

`SELECT luajit_i('return x*2', 21)` on a **literal** folds at bind time —
you are measuring the binder, not the UDF. Benchmark with **column args**:

```sql
SELECT count(*) FROM range(1000000) t(v), LATERAL (SELECT luajit_i('return v*2', v));
```

## Anonymous form registers, it doesn't call

`luajit_s('return "hi"')` (single arg) returns `function: 0x...` — that is
the anonymous **compile** path. To call, pass an argument so the expression
binds `x`: `luajit_s('return "hello " .. x', 'world')`.

## quick_compile creates a MACRO, not a native UDF

`SELECT f(3, 4)` works for scalars, but struct parameters are cast to
VARCHAR by the macro. Call through the resolver instead:
`luajit_s('f', {x:3, y:4})`.

## Security levels & what breaks after switching

`restricted` removes `package`/`load*`/`debug` and guards `require`
(ffi-only) — code using `require('lpeg')` etc. will start failing. Guards
are **one-way per session** (fail-closed): switching back to `full` needs a
new session. Levels reach all threads' states lazily (v0.34): a UDF on a
thread whose state predates the switch gets restricted on first touch.

Guard functions are C closures, not nil — truthiness probes
(`if io.popen then ...`) do NOT detect the restriction; pcall the call.

## FFI

- LuaJIT does not register `ffi` as a global — always `local ffi = require("ffi")`.
- Under `restricted`, `ffi.load` only accepts absolute paths inside
  `~/.duckdb/luajit-ffi/` (create the dir first).
- glibc fortify: if you extend the C side, `realpath` buffers must be
  ≥ PATH_MAX (4096).

## The SQL callback bridge (`_duckdb_query`)

- **Allowed**: a UDF querying its own table (MVCC snapshot read), concurrent
  bridge use from parallel UDFs.
- **Hard error (by design)**: reentrancy — a UDF that itself runs inside a
  bridge-issued query calling the bridge again (`reentrancy blocked`).
- The bridge reads integer columns at true physical width since v0.30;
  older docs' "all ints read as int64" caveat is obsolete.

## luajit_table on macOS/Windows

Table-function serialization requires `SET threads=1` on those platforms
(parallel generator hand-off). Linux is unaffected. This is why those
platforms run the smoke suite rather than full sqllogictest in CI — see
the platform matrix in README.

## install / INDEX.v2

- `install` verifies SHA-256 against INDEX.v2 **fail-closed** — after
  upstream lib changes, a stale local cache errors instead of running stale
  code. `mode := 'upgrade'` refreshes (cache + INDEX.v2, then reinstall).
- Cache lives in `~/.duckdb/luajit-libs/` — when reproducing install
  issues, clear **both** `<name>.lua` and `INDEX.v2` there.
- URL `source :=` overrides are fetched verbatim (no hash check) — that's
  the explicit-user-sources path.

## Debugging Lua inside DuckDB

`io.stderr:write('DBG ', tostring(x), '\n')` still reaches your terminal
under `full`; under `restricted`/`sandbox` use returned values or
`last_error` instead.
