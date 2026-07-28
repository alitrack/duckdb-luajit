/*
 * luajit — persistent connection for UDF registration
 * SPDX-License-Identifier: MIT
 */

#include "duckdb_extension.h"
#include "luajit_module.h"

DUCKDB_EXTENSION_EXTERN

DUCKDB_EXTENSION_ENTRYPOINT(
    duckdb_connection connection,
    duckdb_extension_info info,
    struct duckdb_extension_access *access)
{
	luajit_register_module_functions(connection, info, access);
	return true;
}
