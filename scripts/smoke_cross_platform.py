# Smoke tests for platforms where the full sqllogictest suite is skipped
# (macOS arm64/x64, Windows). ~10 assertion-level statements covering the
# extension's core surfaces. Runs against a pip-installed duckdb of the same
# version the extension was built for. Review P1-1.
#
# Usage (CI):
#   pip install duckdb==<version>
#   python3 scripts/smoke_cross_platform.py <path/to/luajit.duckdb_extension>
import sys
import duckdb

ext_path = sys.argv[1] if len(sys.argv) > 1 else "build/release/luajit.duckdb_extension"
fails = []


def check(name, sql, expect, con=None, exact=False):
    global con_g
    c = con or con_g
    try:
        rows = c.execute(sql).fetchall()
        got = rows[0][0] if rows and len(rows[0]) == 1 else rows
        if exact:
            ok = str(got) == str(expect)
        else:
            ok = str(expect) in str(got) or str(got) == str(expect)
    except Exception as e:  # noqa: BLE001
        got = f"EXC: {e}"
        ok = False
    status = "PASS" if ok else "FAIL"
    print(f"[{status}] {name}: got={got!r} expect~{expect!r}")
    if not ok:
        fails.append(name)


con_g = duckdb.connect(":memory:", config={"allow_unsigned_extensions": True})
con_g.execute(f"LOAD '{ext_path}'")

# 1. anonymous scalar UDF (int)
check("anon int", "SELECT luajit_i('return 21*2')", 42)
# 2. anonymous string UDF
check("anon str", "SELECT luajit_s('return string.upper(x)', 'ok')", "OK")
# 3. quick_compile macro + call (arity + return-type probes)
con_g.execute("SELECT message FROM luajit_module(mode := 'quick_compile', sql_name := 'smk_add', "
              "source := 'return function(a, b) return a + b end')")
check("quick_compile", "SELECT smk_add(3, 4)", 7, exact=True)
# 4. registered table-valued use (luajit_s with column arg, 5 rows)
check("column udf", "SELECT count(*) FROM range(5) t(i), LATERAL (SELECT luajit_i('return i*2', i) r) ", 5)
# 5. security levels: restricted blocks io.popen
con_g.execute("SELECT message FROM luajit_module(mode := 'security', source := 'restricted')")
check("restricted blocks popen", "SELECT luajit('io.popen(\"echo hi\") return \"ran\"')",
      "blocked by luajit security level")
# 6. security levels: os.date survives restricted
check("os.date survives", "SELECT luajit('return os.date(\"%Y\")') >= '2026'", True)
# 7. sandbox removes io
con_g.execute("SELECT message FROM luajit_module(mode := 'security', source := 'sandbox')")
check("sandbox removes io", "SELECT luajit('return type(io)')", "nil")
# 8. bridge round-trip (reentrancy boundary: legal query allowed)
con_g.execute("CREATE TABLE smk_t AS SELECT range i FROM range(5)")
con_g.execute("SELECT message FROM luajit_module(mode := 'quick_compile', sql_name := 'smk_cnt', "
              "source := 'return function(x) local r = _duckdb_query(\"SELECT count(*) c FROM smk_t\") "
              "return tonumber(r[1].c) end')")
check("bridge count", "SELECT smk_cnt(0)", 5, exact=True)
# 9. sha256 mode anchor (FIPS vector)
check("sha256 anchor", "SELECT (SELECT message FROM luajit_module(mode := 'sha256', source := 'abc'))",
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
# 10. back to full level + UDF still works (state discipline)
con_g.execute("SELECT message FROM luajit_module(mode := 'security', source := 'full')")
check("full restores udf", "SELECT smk_add(40, 2)", 42, exact=True)

print()
if fails:
    print(f"SMOKE FAILED: {len(fails)}: {fails}")
    sys.exit(1)
print("SMOKE OK: 10/10")
