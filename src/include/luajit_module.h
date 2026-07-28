/*
 * luajit — DuckDB LuaJIT UDF Extension
 * SPDX-License-Identifier: MIT
 *
 * Public header: type definitions and function declarations.
 */

#ifndef LUAJIT_MODULE_H
#define LUAJIT_MODULE_H

#include "duckdb_extension.h"

/* Forward declaration — full LuaJIT headers only needed in bridge/module units */
struct lua_State;
typedef struct lua_State lua_State;

/*
 * UDF extra info — attached to each registered Lua UDF.
 * Holds the Lua state, function reference, and type metadata.
 */
typedef struct {
	lua_State *L;
	int func_ref;               /* Lua registry index of the compiled function */
	duckdb_logical_type return_type;
	duckdb_logical_type *arg_types;
	idx_t arg_count;
} luajit_udf_extra_t;

/* Register all extension functions on a connection */
void luajit_register_module_functions(duckdb_connection connection);

/* Chunk-level scalar UDF executor */
void luajit_execute_chunk_scalar(
    duckdb_function_info info, duckdb_data_chunk input,
    duckdb_vector output);

#endif /* LUAJIT_MODULE_H */
