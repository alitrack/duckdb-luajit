/*
 * luajit — DuckDB LuaJIT UDF Extension
 * SPDX-License-Identifier: MIT
 *
 * luajit_module.c: Registers luajit() scalar function for evaluating
 * Lua expressions inline, and luajit_module() table function for
 * compiling named Lua UDFs.
 */

#include "duckdb_extension.h"
#include "luajit_module.h"

DUCKDB_EXTENSION_EXTERN

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <string.h>
#include <stdlib.h>

/* ===== Shared LuaJIT state ===== */

static lua_State *g_lua_state = NULL;

static void ensure_lua_state(void) {
    if (!g_lua_state) {
        g_lua_state = luaL_newstate();
        if (g_lua_state) luaL_openlibs(g_lua_state);
    }
}

/* ===== luajit() scalar function ===== */

static void luajit_scalar_func(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    ensure_lua_state();
    lua_State *L = g_lua_state;
    if (!L) {
        duckdb_vector_assign_string_element(output, 0, "error: no Lua state");
        return;
    }

    idx_t row_count = duckdb_data_chunk_get_size(input);
    for (idx_t row = 0; row < row_count; row++) {
        duckdb_vector script_vec = duckdb_data_chunk_get_vector(input, 0);
        duckdb_string_t script_s =
            ((duckdb_string_t *)duckdb_vector_get_data(script_vec))[row];

        if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(script_vec), row)) {
            duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
            continue;
        }

        const char *src = duckdb_string_t_data(&script_s);
        idx_t src_len = duckdb_string_t_length(script_s);

        if (luaL_loadbuffer(L, src, src_len, "luajit") != LUA_OK) {
            duckdb_vector_assign_string_element(output, row, lua_tostring(L, -1));
            lua_pop(L, 1);
            continue;
        }

        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            duckdb_vector_assign_string_element(output, row, lua_tostring(L, -1));
            lua_pop(L, 1);
            continue;
        }

        /* Convert result to string */
        if (lua_isnil(L, -1)) {
            duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
        } else if (lua_isstring(L, -1)) {
            duckdb_vector_assign_string_element(output, row, lua_tostring(L, -1));
        } else {
            lua_getglobal(L, "tostring");
            lua_pushvalue(L, -2);
            lua_pcall(L, 1, 1, 0);
            duckdb_vector_assign_string_element(output, row,
                lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
            lua_pop(L, 1);
        }
        lua_pop(L, 1); /* pop result */
    }
}

/* ===== Registration ===== */

void luajit_register_module_functions(duckdb_connection connection) {
    ensure_lua_state();

    /* luajit() scalar function */
    duckdb_scalar_function f = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(f, "luajit");
    duckdb_scalar_function_set_function(f, luajit_scalar_func);
    duckdb_scalar_function_set_return_type(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_scalar_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_register_scalar_function(connection, f);
    duckdb_destroy_scalar_function(&f);
}
