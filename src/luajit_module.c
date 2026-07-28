/*
 * luajit — DuckDB LuaJIT UDF Extension
 * SPDX-License-Identifier: MIT
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
    if (!L) { duckdb_vector_assign_string_element(output, 0, "error: no Lua state"); return; }

    idx_t row_count = duckdb_data_chunk_get_size(input);
    for (idx_t row = 0; row < row_count; row++) {
        duckdb_vector script_vec = duckdb_data_chunk_get_vector(input, 0);
        duckdb_string_t script_s = ((duckdb_string_t *)duckdb_vector_get_data(script_vec))[row];
        if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(script_vec), row)) {
            duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
            continue;
        }
        if (luaL_loadbuffer(L, duckdb_string_t_data(&script_s),
                            duckdb_string_t_length(script_s), "luajit") != LUA_OK) {
            duckdb_vector_assign_string_element(output, row, lua_tostring(L, -1));
            lua_pop(L, 1); continue;
        }
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            duckdb_vector_assign_string_element(output, row, lua_tostring(L, -1));
            lua_pop(L, 1); continue;
        }
        if (lua_isnil(L, -1)) {
            duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
        } else if (lua_isstring(L, -1)) {
            duckdb_vector_assign_string_element(output, row, lua_tostring(L, -1));
        } else {
            lua_getglobal(L, "tostring"); lua_pushvalue(L, -2); lua_pcall(L, 1, 1, 0);
            const char *s = lua_tostring(L, -1);
            duckdb_vector_assign_string_element(output, row, s ? s : "?");
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
}

/* ===== luajit_module() table function ===== */

typedef struct {
    char *mode;
    char *source;
    char *sql_name;
    char *error;
    bool ok;
} module_bind_t;

typedef struct {
    bool produced;
} module_state_t;

static void module_bind(duckdb_bind_info info) {
    module_bind_t *d = (module_bind_t *)duckdb_malloc(sizeof(module_bind_t));
    memset(d, 0, sizeof(*d));
    d->mode = strdup("info");

    duckdb_value v;
    v = duckdb_bind_get_named_parameter(info, "mode");
    if (v) { free(d->mode); d->mode = strdup(duckdb_get_varchar(v)); duckdb_destroy_value(&v); }
    v = duckdb_bind_get_named_parameter(info, "source");
    if (v) { d->source = strdup(duckdb_get_varchar(v)); duckdb_destroy_value(&v); }
    v = duckdb_bind_get_named_parameter(info, "sql_name");
    if (v) { d->sql_name = strdup(duckdb_get_varchar(v)); duckdb_destroy_value(&v); }

    duckdb_bind_add_result_column(info, "ok", duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN));
    duckdb_bind_add_result_column(info, "mode", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_bind_add_result_column(info, "message", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));

    duckdb_bind_set_bind_data(info, d, free);
}

static void module_init(duckdb_init_info info) {
    module_bind_t *d = (module_bind_t *)duckdb_init_get_bind_data(info);
    if (!d) return;

    module_state_t *state = (module_state_t *)duckdb_malloc(sizeof(module_state_t));
    memset(state, 0, sizeof(*state));
    duckdb_init_set_init_data(info, state, free);

    if (strcmp(d->mode, "info") == 0) {
        d->ok = true;
        return;
    }

    if (strcmp(d->mode, "compile") != 0) {
        d->error = strdup("unsupported mode; use 'compile' or 'info'");
        return;
    }

    if (!d->source || !d->sql_name) {
        d->error = strdup("source and sql_name required for compile");
        return;
    }

    ensure_lua_state();
    lua_State *L = g_lua_state;
    if (!L) { d->error = strdup("no Lua state"); return; }

    if (luaL_loadstring(L, d->source) != LUA_OK) {
        d->error = strdup(lua_tostring(L, -1)); lua_pop(L, 1); return;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        d->error = strdup(lua_tostring(L, -1)); lua_pop(L, 1); return;
    }
    if (!lua_isfunction(L, -1)) {
        d->error = strdup("source must return a function"); lua_pop(L, 1); return;
    }

    int fref = luaL_ref(L, LUA_REGISTRYINDEX);
    /* v0.1: UDF registration needs a connection (not available in init callback).
     * The compiled function is stored in the Lua registry for future use.
     * For now, luajit() scalar function provides inline evaluation. */
    d->error = strdup("v0.1: compile mode stores function; use luajit() for inline eval");
    d->ok = true;
}

static void module_func(duckdb_function_info info, duckdb_data_chunk output) {
    module_bind_t *d = (module_bind_t *)duckdb_function_get_bind_data(info);
    module_state_t *state = (module_state_t *)duckdb_function_get_init_data(info);
    if (!d || !state || state->produced) {
        duckdb_data_chunk_set_size(output, 0);
        return;
    }

    duckdb_data_chunk_set_size(output, 1);
    bool *ok_col = (bool *)duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 0));
    ok_col[0] = d->ok;
    duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(output, 1), 0,
        d->mode ? d->mode : "?");
    duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(output, 2), 0,
        d->error ? d->error : (d->ok ? "OK" : "ERROR"));
    state->produced = true;
}

/* ===== Registration ===== */

void luajit_register_module_functions(duckdb_connection connection) {
    ensure_lua_state();

    duckdb_scalar_function f = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(f, "luajit");
    duckdb_scalar_function_set_function(f, luajit_scalar_func);
    duckdb_scalar_function_set_return_type(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_scalar_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_register_scalar_function(connection, f);
    duckdb_destroy_scalar_function(&f);

    duckdb_table_function tf = duckdb_create_table_function();
    duckdb_table_function_set_name(tf, "luajit_module");
    duckdb_table_function_add_named_parameter(tf, "mode", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_table_function_add_named_parameter(tf, "source", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_table_function_add_named_parameter(tf, "sql_name", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_table_function_set_bind(tf, module_bind);
    duckdb_table_function_set_init(tf, module_init);
    duckdb_table_function_set_function(tf, module_func);
    duckdb_register_table_function(connection, tf);
    duckdb_destroy_table_function(&tf);
}
