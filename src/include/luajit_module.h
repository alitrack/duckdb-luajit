/*
 * luajit — DuckDB LuaJIT UDF Extension
 * SPDX-License-Identifier: MIT
 *
 * Public header for the luajit extension.
 */

#ifndef LUAJIT_MODULE_H
#define LUAJIT_MODULE_H

#include "duckdb_extension.h"

/*
 * Register luajit_module() table function, system paths probe,
 * and pointer/memory helper functions on the given connection.
 */
void luajit_register_module_functions(duckdb_connection connection);

#endif /* LUAJIT_MODULE_H */
