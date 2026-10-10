---
name: duckdb-luajit-security
description: >
  Choose and configure duckdb-luajit security levels (full / restricted /
  sandbox) for safe deployment, understand FFI allowlists, remote-library
  sha256 verification, and what each level blocks. Use when running luajit
  UDFs on shared servers, with untrusted input, or in service deployments.
version: 0.33.0
---

# duckdb-luajit: security levels & deployment

Three levels, set per session or via environment for services:

```sql
SELECT * FROM luajit_module(mode := 'security', source := 'full');        -- or restricted / sandbox
```

or `LUAJIT_SECURITY_LEVEL=restricted` env var before starting DuckDB.

| Level | Use when | What it blocks |
|---|---|---|
| `full` (default) | local dev, trusted scripts | nothing (files, FFI, shell all available) |
| `restricted` | shared / reporting environments | shell exec (`io.popen`, `os.execute`), env/temp-file tampering, `require` (except `ffi`), `ffi.load` outside `~/.duckdb/luajit-ffi/` allowlist |
| `sandbox` | fully untrusted input | `io`/`ffi`/`require`/`load*` removed, `os` reduced to date/time |

## FFI allowlist (restricted level)

Native libraries load only from `~/.duckdb/luajit-ffi/` — drop the .so there
to permit exactly one binary. `ffi.load('/etc/passwd')` style paths fail
closed.

## Remote library integrity

Installs from duckdb-luajit-libs are **sha256-verified against INDEX.v2**
both after download and on cache hit — a tampered source fails closed.
`mode := 'upgrade'` refreshes INDEX.v2 when the remote rotates.

## Deployment checklist

1. Services / multi-tenant: start with `restricted` (env var beats session
   setting — set it before the process starts).
2. Untrusted user-supplied Lua source: `sandbox`.
3. Keep only vetted binaries in the FFI allowlist dir; treat it as a
   capability grant.
4. Report vulnerabilities per SECURITY.md (coordinated disclosure).
