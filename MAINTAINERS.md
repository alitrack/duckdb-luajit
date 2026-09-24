# Maintainers

## Current maintainers

| Name | GitHub | Role | Scope |
|---|---|---|---|
| alitrack | [@alitrack](https://github.com/alitrack) | Lead maintainer | Core extension (`duckdb-luajit`), libs repo (`duckdb-luajit-libs`), releases |

**Bus factor is currently 1.** This document exists to lower the entry bar for
co-maintainers — see "Becoming a maintainer" below.

## Responsibilities

A maintainer is expected to:

1. **Review and merge PRs** in their scope (core extension vs. libs repo).
2. **Triage issues**: label, reproduce, answer. Bug reports with a SQL
   reproducer get priority.
3. **Make releases** (see "Release process" — it is fully automated; the
   maintainer only pushes a tag).
4. **Keep `INDEX.v2` in sync** in the libs repo: after ANY change to a lib
   file, run `python3 scripts/gen_index_v2.py` from the repo root and commit
   the result. Install verification is sha256-based and **fails closed** —
   a stale INDEX.v2 breaks `install` for everyone.
5. **Respond to security reports** (see SECURITY.md) within 7 days.

## Release process (fully automated)

Releases require no manual artifact handling:

1. All tests green on `master` (`sqllogictest-linux` job runs the full
   suite; the main distribution pipeline is build-only for other platforms).
2. Update `README.md` changelog section + version constants if needed.
3. `git tag vX.Y.Z && git push origin vX.Y.Z`
4. CI does the rest: multi-platform build (linux amd64/arm64, macOS
   amd64/arm64, Windows MSVC) → GitHub Release with platform-suffixed
   assets. Verify the Release page lists the expected assets.

Anyone with push access can execute this. If CI is red for reasons unrelated
to the change (flaky runner), re-run the job; do not bypass failing tests.

## Becoming a maintainer

Sustained contributions are the path: several merged PRs (or consistently
high-quality issue triage) over ~2 months. Areas where help is most wanted:

- **libs repo** (privacy / medical / financial / etl libs) — lowest entry
  bar; libs are pure Lua, testable with plain SQL.
- **sqllogictest coverage** on macOS arm64 / Windows (platform-specific
  behaviour differences are documented in `test/sql/luajit_bridge.test`).
- **benchmark harness** (see the 90-day review, P1-2).

Express interest in an issue or PR; the lead maintainer grants write access
after a private chat (channel: GitHub issue or the repo's discussions).

## Emeritus process / unreachable maintainer

- If a maintainer is inactive for **3 months** with no response on
  GitHub mentions, they move to emeritus (write access retained, no release
  duty).
- If the **lead maintainer is unreachable for 3 months** and no co-maintainer
  has push access, the fallback is a fork under a new name, clearly credited
  to the original project. The license (MIT) explicitly permits this — this
  paragraph is the social permission that matches it.

## Communication with upstream DuckDB

- The extension targets the **C API** (`duckdb_extension.h`), not the C++ API.
  Breaking C API changes are announced in
  [duckdb/duckdb](https://github.com/duckdb/duckdb) releases; the maintainer
  bumping `TARGET_DUCKDB_VERSION` in the Makefile handles them at upgrade
  time.
- Community-extension listing: PR to
  [duckdb/community-extensions](https://github.com/duckdb/community-extensions)
  (a fork with an open PR is kept in `git remote -v` output as `fork`).
  Status of that PR is part of the roadmap.
