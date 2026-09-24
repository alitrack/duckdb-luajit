#!/usr/bin/env python3
"""duckdb-luajit reproducible micro-benchmarks (review P1-2).

Run:  python3 benchmark/run.py [--quick]
Output: markdown table on stdout + JSON rows appended to benchmark/results.jsonl
        (git-commit the jsonl to keep a history; CI writes results.ci.jsonl)

Design rules (from the independent review):
  - pass COLUMN parameters, never constants (defeats constant folding)
  - compare batch (table-mode) vs row (per-row scalar) where applicable
  - FFI call overhead vs pure-Lua baseline
  - parallel scaling via SET threads
All numbers are throughput on THIS machine; the point is reproducibility
(same script, same data) not absolute speed.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
EXT = os.environ.get("LUAJIT_EXT", os.path.join(HERE, "..", "build", "release", "luajit.duckdb_extension"))
N_DEFAULT = 1_000_000
N_QUICK = 100_000

PY_DUCKDB = os.path.join(HERE, "..", "configure", "venv", "bin", "python3")


def connect():
    import duckdb  # inside venv
    con = duckdb.connect(config={"allow_unsigned_extensions": "true",
                                 "threads": "1"})
    con.execute(f"LOAD '{os.path.abspath(EXT)}'")
    return con


def timed(con, sql, repeats=3):
    """Median wall time of con.execute(sql).fetchall(), warmed up once."""
    con.execute(sql).fetchall()  # warmup
    best: float | None = None
    for _ in range(repeats):
        t0 = time.perf_counter()
        con.execute(sql).fetchall()
        dt = time.perf_counter() - t0
        best = dt if best is None else min(best, dt)
    return best if best is not None else 0.0


def bench(con, name, sql, n, rows_out=1):
    dt = timed(con, sql)
    rps = n / dt
    print(f"| {name} | {n:,} | {dt*1000:8.1f} ms | {rps/1e6:6.2f} M rows/s |")
    return {"name": name, "rows": n, "ms": round(dt * 1000, 1),
            "m_rows_per_s": round(rps / 1e6, 2)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args()
    n = N_QUICK if args.quick else N_DEFAULT

    con = connect()
    print(f"| benchmark | rows | time | throughput |")
    print(f"|---|---|---|---|")
    print(f"| env | duckdb {con.execute('select version()').fetchone()[0]}, "
          f"threads=1, n={n:,} | | |")
    results = []

    # Data: column parameter (NOT a constant) — constant-folding defense.
    con.execute(f"CREATE TABLE t AS SELECT (range % 1000) AS v FROM range({n})")

    # 1. Registered scalar UDF vs anonymous (row mode, column arg)
    con.execute("""SELECT * FROM luajit_module(mode := 'quick_compile', sql_name := 'lj_x2',
        source := 'return function(x) return x * 2 end')""")
    results.append(bench(con, "registered scalar x2 (row)", f"SELECT sum(lj_x2(v)) FROM t", n))
    results.append(bench(con, "anonymous expr x*2 (row)", f"SELECT sum(luajit_i('return x * 2', v)) FROM t", n))

    # 2. String UDF (heavier per-row work) — column arg
    con.execute("""SELECT * FROM luajit_module(mode := 'quick_compile', sql_name := 'lj_fmt',
        source := 'return function(x) return string.format("%05d", x) end')""")
    results.append(bench(con, "scalar string.format (row)", f"SELECT count(DISTINCT lj_fmt(v)) FROM t", n))

    # 3. Bridge: UDF querying DuckDB from Lua (stored-procedure shape)
    con.execute(f"CREATE TABLE agg AS SELECT g, sum(v) s FROM (SELECT range % 10 g, range v FROM range({min(n,100000)})) GROUP BY g")
    con.execute("""SELECT * FROM luajit_module(mode := 'quick_compile', sql_name := 'lj_lookup',
        source := 'return function(x) local r = _duckdb_query("SELECT s FROM agg WHERE g = 0") return r[1].s end')""")
    results.append(bench(con, "bridge lookup (100k)", "SELECT count(lj_lookup(range)) FROM range(1000)", 1000))

    # 4. Parallel scaling: threads=1 vs 4 on the row-mode scalar
    for th in (1, 4):
        con.execute(f"SET threads = {th}")
        results.append(bench(con, f"scalar x2 threads={th}", f"SELECT sum(lj_x2(v)) FROM t", n))
    con.execute("SET threads = 1")

    out = os.path.join(HERE, "results.jsonl")
    with open(out, "a") as f:
        for r in results:
            f.write(json.dumps({"ts": time.strftime("%Y-%m-%dT%H:%M:%S"), **r}) + "\n")
    print(f"\nappended {len(results)} rows -> {out}")


if __name__ == "__main__":
    main()
