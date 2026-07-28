/*
 * luajit — DuckDB LuaJIT UDF Extension
 * SPDX-License-Identifier: MIT
 *
 * luajit_module.c: Control plane for LuaJIT UDF compilation and registration.
 *
 * Strategy:
 * - luajit_module(...) is the control plane: it stages session-scoped Lua sources,
 *   parses type signatures, registers Lua functions as DuckDB scalar UDFs.
 * - LuaJIT FFI eliminates the need for C wrapper codegen — DuckDB C API
 *   functions are exposed directly to Lua via ffi.cdef / ffi.C.*.
 * - Type bridge maps DuckDB logical types to LuaJIT FFI ctypes for
 *   zero-copy vector access during UDF execution.
 */

#include "duckdb_extension.h"
#include "luajit_module.h"

/*
 * Register the luajit_module table function and related helpers.
 * Placeholder — full implementation to follow.
 */
void luajit_register_module_functions(duckdb_connection connection) {
	/* TODO: Register luajit_module table function */
	/* TODO: Register luajit_system_paths table function */
	/* TODO: Register pointer/memory helper functions */
}
