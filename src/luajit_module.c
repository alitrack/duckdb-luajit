/*
 * luajit — DuckDB LuaJIT UDF Extension  v0.3
 * SPDX-License-Identifier: MIT
 *
 * Architecture: UDFs are stored as Lua globals. Access via luajit():
 *   SELECT luajit('return my_udf(42)')
 * No per-UDF DuckDB function registration needed — single luajit() entry point.
 */

#include "duckdb_extension.h"
#include "luajit_module.h"

DUCKDB_EXTENSION_EXTERN

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <string.h>
#include <stdlib.h>

/* ===== State ===== */
static lua_State *g_lua = NULL;

static void ensure_lua(void) {
    if (!g_lua) { g_lua = luaL_newstate(); if (g_lua) luaL_openlibs(g_lua); }
}

/* ===== luajit(expr) — evaluate Lua expression ===== */

static void f_luajit(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    ensure_lua(); lua_State *L = g_lua;
    if (!L) return;
    idx_t n = duckdb_data_chunk_get_size(input);
    for (idx_t r = 0; r < n; r++) {
        duckdb_vector sv = duckdb_data_chunk_get_vector(input, 0);
        duckdb_string_t ss = ((duckdb_string_t *)duckdb_vector_get_data(sv))[r];
        if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(sv), r))
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output),r); continue; }
        int ok = luaL_loadbuffer(L, duckdb_string_t_data(&ss),
                                  duckdb_string_t_length(ss), "lj");
        ok = ok || lua_pcall(L, 0, 1, 0);
        if (ok) { duckdb_vector_assign_string_element(output, r, lua_tostring(L,-1));
                  lua_pop(L,1); continue; }
        if (lua_isnil(L, -1)) duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output),r);
        else { lua_getglobal(L,"tostring");lua_pushvalue(L,-2);lua_pcall(L,1,1,0);
               duckdb_vector_assign_string_element(output, r, lua_tostring(L,-1));
               lua_pop(L,1); }
        lua_pop(L, 1);
    }
}

/* ===== luajit_module() table function ===== */

typedef struct { char *mode, *source, *sql_name, *msg; bool ok; } bind_t;
typedef struct { bool done; } state_t;

static void tf_bind(duckdb_bind_info info) {
    bind_t *d = (bind_t *)duckdb_malloc(sizeof(bind_t));
    memset(d, 0, sizeof(*d)); d->mode = strdup("info");
    #define G(n,f) { duckdb_value v=duckdb_bind_get_named_parameter(info,n); \
        if(v){free(d->f);d->f=strdup(duckdb_get_varchar(v));duckdb_destroy_value(&v);} }
    G("mode", mode); G("source", source); G("sql_name", sql_name);
    #undef G
    duckdb_bind_add_result_column(info, "ok", duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN));
    duckdb_bind_add_result_column(info, "mode", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_bind_add_result_column(info, "message", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_bind_set_bind_data(info, d, free);
}

static void tf_init(duckdb_init_info info) {
    bind_t *d = (bind_t *)duckdb_init_get_bind_data(info);
    if (!d) return;
    state_t *s = (state_t *)duckdb_malloc(sizeof(state_t));
    memset(s, 0, sizeof(*s));
    duckdb_init_set_init_data(info, s, free);

    if (strcmp(d->mode, "info") == 0) { d->ok = true; return; }
    if (strcmp(d->mode, "compile") != 0) { d->msg = strdup("bad mode"); return; }
    if (!d->source || !d->sql_name) { d->msg = strdup("need source+sql_name"); return; }

    ensure_lua(); lua_State *L = g_lua;
    if (!L) { d->msg = strdup("no Lua"); return; }

    /* Compile source → function */
    if (luaL_loadstring(L, d->source) != LUA_OK)
        { d->msg = strdup(lua_tostring(L,-1)); lua_pop(L,1); return; }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK)
        { d->msg = strdup(lua_tostring(L,-1)); lua_pop(L,1); return; }
    if (!lua_isfunction(L, -1))
        { d->msg = strdup("must return a function"); lua_pop(L,1); return; }

    /* Store as Lua global: _G[sql_name] = function */
    lua_setglobal(L, d->sql_name);

    d->ok = true; d->msg = NULL;
}

static void tf_func(duckdb_function_info info, duckdb_data_chunk output) {
    bind_t *d = (bind_t *)duckdb_function_get_bind_data(info);
    state_t *s = (state_t *)duckdb_function_get_init_data(info);
    if (!d || !s || s->done) { duckdb_data_chunk_set_size(output, 0); return; }
    duckdb_data_chunk_set_size(output, 1);
    ((bool *)duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 0)))[0] = d->ok;
    duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(output,1),0,d->mode?d->mode:"?");
    duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(output,2),0,
        d->msg?d->msg:(d->ok?"OK":"ERROR"));
    s->done = true;
}

/* ===== Init ===== */

void luajit_register_module_functions(
    duckdb_connection conn, duckdb_extension_info ei, struct duckdb_extension_access *acc)
{
    (void)ei; (void)acc;
    ensure_lua();

    duckdb_scalar_function f = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(f, "luajit");
    duckdb_scalar_function_set_function(f, f_luajit);
    duckdb_scalar_function_set_return_type(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_scalar_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_register_scalar_function(conn, f);
    duckdb_destroy_scalar_function(&f);

    duckdb_table_function t = duckdb_create_table_function();
    duckdb_table_function_set_name(t, "luajit_module");
    duckdb_table_function_add_named_parameter(t, "mode", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_table_function_add_named_parameter(t, "source", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_table_function_add_named_parameter(t, "sql_name", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_table_function_set_bind(t, tf_bind);
    duckdb_table_function_set_init(t, tf_init);
    duckdb_table_function_set_function(t, tf_func);
    duckdb_register_table_function(conn, t);
    duckdb_destroy_table_function(&t);
}
