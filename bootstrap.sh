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

# LuaJIT — git submodule, init if needed
echo "Initializing submodules..."
git submodule update --init

echo "=== Done. Run: make configure && make release ==="
