/*
 * luajit — DuckDB LuaJIT UDF Extension
 * SPDX-License-Identifier: MIT
 *
 * luajit_bridge.c: Runtime bridge between DuckDB vectors and LuaJIT FFI.
 *
 * Uses LuaJIT FFI to access DuckDB vector data directly — no C glue code,
 * no generated wrappers. The Lua UDF receives DuckDB pointers via ffi.cdef
 * and reads/writes through them.
 */

#include "duckdb_extension.h"
#include "luajit_module.h"

/* Placeholder — full bridge implementation to follow */
