/*
 * luajit — DuckDB LuaJIT UDF Extension
 * SPDX-License-Identifier: MIT
 */

#ifndef LUAJIT_MODULE_H
#define LUAJIT_MODULE_H

#include "duckdb_extension.h"

struct lua_State;
typedef struct lua_State lua_State;

typedef struct {
	lua_State *L;
	int func_ref;
	duckdb_logical_type return_type;
	duckdb_logical_type *arg_types;
	idx_t arg_count;
} luajit_udf_extra_t;

void luajit_register_module_functions(
    duckdb_connection temp_conn,
    duckdb_extension_info info,
    struct duckdb_extension_access *access);

#endif
