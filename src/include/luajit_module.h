/*
 * luajit — DuckDB LuaJIT UDF Extension
 * SPDX-License-Identifier: MIT
 */

#ifndef LUAJIT_MODULE_H
#define LUAJIT_MODULE_H

#include "duckdb_extension.h"

void luajit_register_module_functions(
    duckdb_connection conn,
    duckdb_extension_info info,
    struct duckdb_extension_access *access);

#endif
