#!/usr/bin/env python3
"""Unified batch-vs-row benchmark for duckdb-luajit — closes the harness gap.

Purpose: produce ONE coherent number set for the paper, on the same data,
same machine, same run, covering:
  A. row-mode scalar (luajit_i) vs chunk-batched (luajit_vi)      -> "1.9x" claim
  B. threads=1 vs 4 scaling (both modes)                          -> "2.6x" claim
  C. pure-SQL expression baseline (defeats "why not just SQL?")
Design rules inherited from benchmark/run.py (column params, warmup+min-of-3).
Output: markdown to stdout + rows appended to benchmark/results.jsonl
"""
import json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
EXT = os.environ.get("LUAJIT_EXT", os.path.join(HERE, "..", "build", "release", "luajit.duckdb_extension"))
N = 1_000_000
PY = os.path.join(HERE, "..", "configure", "venv", "bin", "python3")

script = r'''
import duckdb, json, os, time
EXT = os.environ["LUAJIT_EXT"]
con = duckdb.connect(config={"allow_unsigned_extensions": "true", "threads": "1"})
con.execute(f"LOAD '{os.path.abspath(EXT)}'")
ver = con.execute("select version()").fetchone()[0]
con.execute("CREATE TABLE t AS SELECT (range % 1000) AS v FROM range(__N__)")

con.execute("SELECT * FROM luajit_module(mode := 'quick_compile', sql_name := 'lj_x2', source := 'return function(x) return x * 2 end')")

def timed(sql, repeats=5):
    con.execute(sql).fetchall()
    best = None
    for _ in range(repeats):
        t0 = time.perf_counter(); con.execute(sql).fetchall(); dt = time.perf_counter() - t0
        best = dt if best is None else min(best, dt)
    return best * 1000

rows = []
def bench(name, sql, threads):
    con.execute(f"SET threads = {threads}")
    ms = timed(sql)
    rows.append({"name": name, "threads": threads, "ms": round(ms, 1)})
    print(f"| {name} | t={threads} | {ms:8.1f} ms | {(__N__/ms*1000/1e6):6.2f} M/s |")

# batch UDF: registered table-in/table-out (chunk-batched path — the 1.9x claim)
con.execute("SELECT * FROM luajit_module(mode := 'quick_compile', sql_name := 'bx2', source := 'return function(xs) local out = {} for i, x in ipairs(xs) do out[i] = x * 2 end return out end')")

for th in (1, 4):
    bench("row-mode luajit_i x*2 (anon)", "SELECT sum(luajit_i('return x * 2', v)) FROM t", th)
    sql_vi = "SELECT sum(luajit_vi('bx2', v)) FROM t"
    bench("batch luajit_vi x*2 (registered)", sql_vi, th)
    bench("pure SQL x*2", "SELECT sum(v*2) FROM t", th)

print("duckdb", ver)
for r in rows:
    print("JSONROW", json.dumps(r))
'''.replace("__N__", str(N))
env = dict(os.environ, LUAJIT_EXT=os.path.abspath(EXT))
p = subprocess.run([PY, "-c", script], capture_output=True, text=True, env=env, cwd=os.path.join(HERE, ".."))
print(p.stdout)
if p.returncode != 0:
    print("STDERR:", p.stderr[-2000:]); sys.exit(1)

# persist
for line in p.stdout.splitlines():
    if line.startswith("JSONROW "):
        r = json.loads(line[8:])
        r["ts"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        with open(os.path.join(HERE, "results.jsonl"), "a") as f:
            f.write(json.dumps(r) + "\n")
print("appended ->", os.path.join(HERE, "results.jsonl"))
