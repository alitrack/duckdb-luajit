# Roadmap

## Done (2026-09, independent-review security pass)

- [x] Supply-chain integrity: `INDEX.v2` sha256 verification for installs
      (fail-closed; cache-hit verification; `upgrade` mode)
- [x] Security levels `full`/`restricted`/`sandbox` + FFI allowlist dir
      + `LUAJIT_SECURITY_LEVEL` env gate
- [x] `_duckdb_query` bridge reentrancy boundary (hard error, no deadlock)
- [x] MAINTAINERS.md / SECURITY.md

## Next

- [ ] Community-extensions listing (PR to duckdb/community-extensions)
- [ ] Co-maintainers for the libs repo (see MAINTAINERS.md — areas wanted)
- [ ] `benchmark/` dir + weekly CI benchmark (review P1-2)
- [ ] SBOM for embedded third-party sources (Fennel, rxi/json.lua, etc.)
- [ ] README first-screen decision tree (review P2-1)

## Explicitly not planned (so nobody waits)

- **WASM support**: LuaJIT has no wasm toolchain story; the extension is
  native-only. If a wasm-side Lua interpreter (wasmoon-style) becomes a
  credible backend, this decision can be revisited — until then: not on
  the roadmap. The C API surface is kept wasm-clean regardless.
- **Signing INDEX.v2 with ed25519**: sha256 pinning already covers the
  realistic threat (upstream repo/account hijack → tampered source).
  End-to-end signing adds key-management burden that a single-maintainer
  project cannot operate safely yet; revisit when co-maintainers exist
  (key custody is the blocker, not the crypto).
