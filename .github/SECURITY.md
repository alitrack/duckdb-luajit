# Security Policy

## Supported versions

The latest `master` and the most recent tagged release.

## Reporting a vulnerability

Open a **private security advisory**: repo → Security → Report a
vulnerability (GitHub's private channel). If that is unavailable, open a
regular issue titled `Security: <one-line summary>` with no exploit details
and a maintainer will provide a private channel.

Please include: DuckDB version, extension version, a SQL reproducer, and
the security level in effect (`full` / `restricted` / `sandbox`).

Response target: **7 days**. Fixes ship as patch releases; credit is given
in the release notes unless you prefer otherwise.

## Security model (v0.33)

Three levels, set via `luajit_module(mode := 'security', source := ...)`
or the `LUAJIT_SECURITY_LEVEL` environment variable (service deployments):

| Level | Shell exec | Filesystem via `io` | FFI | `_duckdb_query` bridge |
|---|---|---|---|---|
| `full` (default) | yes | yes | unrestricted `ffi.load` | yes |
| `restricted` | **blocked** (hard error) | no `os.getenv/remove/rename/tmpname` | `ffi.load` **only from `~/.duckdb/luajit-ffi/`** (realpath-checked allowlist) | yes |
| `sandbox` | **blocked** | `io` removed entirely | removed | yes (documented boundary) |

Bridge reentrancy: a UDF executing inside bridge-issued SQL that calls back
into the bridge gets an immediate hard error (documented in
`src/luajit_module.c`, regression-tested in
`test/sql/luajit_bridge_boundary.test`).

## Supply-chain integrity

Remote lib installs (`luajit_module(mode := 'install')`) are verified
against the `INDEX.v2` sidecar in
[duckdb-luajit-libs](https://github.com/alitrack/duckdb-luajit-libs):
per-lib `sha256` checked after download **and on cache hit** (cache
poisoning defense). Mismatch = hard failure, nothing written to the cache.
`upgrade` clears the poisoned cache and re-fetches. Older extensions that
only read the 2-column `INDEX` keep working unchanged.

## Known limitations

- The default level is `full` (backward compatibility). Deployments running
  untrusted SQL must explicitly set `restricted` or `sandbox`.
- `sandbox` keeps the `_duckdb_query` bridge: a malicious UDF can read any
  table the database connection can read. If that matters for your threat
  model, run untrusted SQL in a separate database file.
- LuaJIT cannot run on WASM builds of DuckDB (toolchain limitation);
  the extension is not available there.
