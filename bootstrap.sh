#!/bin/bash
# luajit bootstrap — clones build dependencies
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== luajit bootstrap ==="

# extension-ci-tools (DuckDB build framework)
if [ ! -d "$DIR/extension-ci-tools" ]; then
    echo "Cloning extension-ci-tools..."
    git clone --depth 1 https://github.com/duckdb/extension-ci-tools.git \
        "$DIR/extension-ci-tools"
fi

# LuaJIT (self-contained Lua runtime)
if [ ! -d "$DIR/third_party/LuaJIT" ]; then
    echo "Cloning LuaJIT..."
    git clone --depth 1 https://github.com/LuaJIT/LuaJIT.git \
        "$DIR/third_party/LuaJIT"
fi

echo "=== Done. Run: make configure && make release ==="
