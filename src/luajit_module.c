/*
 * luajit — DuckDB LuaJIT UDF Extension
 * SPDX-License-Identifier: MIT
 *
 * v0.2: Full UDF compilation and registration.
 *   - luajit(expr): inline eval
 *   - luajit_module(compile): register named Lua UDF as DuckDB scalar function
 */

#include "duckdb_extension.h"
#include "luajit_module.h"

DUCKDB_EXTENSION_EXTERN

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <string.h>
#include <stdlib.h>

/* ===== Shared state ===== */
static lua_State          *g_lua  = NULL;
static duckdb_connection   g_conn = NULL;

/* Simple hash table: name -> Lua func_ref */
#define UDF_MAX 256
typedef struct { char *name; int fref; } udf_entry_t;
static udf_entry_t g_udfs[UDF_MAX];
static int g_udf_count = 0;

static int udf_find(const char *name) {
    for (int i = 0; i < g_udf_count; i++)
        if (strcmp(g_udfs[i].name, name) == 0) return g_udfs[i].fref;
    return LUA_NOREF;
}
static void udf_put(const char *name, int fref) {
    if (g_udf_count < UDF_MAX) {
        g_udfs[g_udf_count].name = strdup(name);
        g_udfs[g_udf_count].fref = fref;
        g_udf_count++;
    }
}

static void ensure_lua(void) {
    if (!g_lua) { g_lua = luaL_newstate(); if (g_lua) luaL_openlibs(g_lua); }
}

/* push a scalar value onto Lua, pop result back as string */
static void lua_to_str(lua_State *L) {
    if (lua_isnil(L, -1)) return;
    if (lua_isstring(L, -1)) return;
    lua_getglobal(L, "tostring"); lua_pushvalue(L, -2);
    lua_pcall(L, 1, 1, 0);
}

/* ===== luajit() scalar ===== */

static void f_luajit(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    ensure_lua(); lua_State *L = g_lua;
    if (!L) { duckdb_vector_assign_string_element(output, 0, "err"); return; }
    idx_t n = duckdb_data_chunk_get_size(input);
    for (idx_t r = 0; r < n; r++) {
        duckdb_vector sv = duckdb_data_chunk_get_vector(input, 0);
        duckdb_string_t ss = ((duckdb_string_t *)duckdb_vector_get_data(sv))[r];
        if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(sv), r))
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), r); continue; }
        if (luaL_loadbuffer(L, duckdb_string_t_data(&ss), duckdb_string_t_length(ss), "lj") != LUA_OK)
            { duckdb_vector_assign_string_element(output, r, lua_tostring(L, -1)); lua_pop(L, 1); continue; }
        if (lua_pcall(L, 0, 1, 0) != LUA_OK)
            { duckdb_vector_assign_string_element(output, r, lua_tostring(L, -1)); lua_pop(L, 1); continue; }
        if (lua_isnil(L, -1))
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), r); }
        else {
            lua_to_str(L);
            duckdb_vector_assign_string_element(output, r, lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }
}

/* ===== UDF executor ===== */

static void f_udf(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    (void)info;
    ensure_lua(); lua_State *L = g_lua;
    if (!L) return;

    idx_t nr = duckdb_data_chunk_get_size(input);
    idx_t nc = duckdb_data_chunk_get_column_count(input);

    /* Get function name from DuckDB's function_info — use a global index table.
     * v0.2: we use the first registered function's ref. Full per-func dispatch in v0.3. */
    int fref = LUA_NOREF;
    if (g_udf_count > 0) fref = g_udfs[g_udf_count - 1].fref;
    if (fref == LUA_NOREF) return;

    for (idx_t r = 0; r < nr; r++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, fref);
        for (idx_t c = 0; c < nc; c++) {
            duckdb_vector v = duckdb_data_chunk_get_vector(input, c);
            duckdb_string_t vs = ((duckdb_string_t *)duckdb_vector_get_data(v))[r];
            if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(v), r))
                lua_pushnil(L);
            else
                lua_pushlstring(L, duckdb_string_t_data(&vs), duckdb_string_t_length(vs));
        }
        if (lua_pcall(L, (int)nc, 1, 0) != LUA_OK) {
            duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), r);
            lua_pop(L, 1); continue;
        }
        if (lua_isnil(L, -1))
            duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), r);
        else {
            lua_to_str(L);
            duckdb_vector_assign_string_element(output, r, lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }
}

/* ===== luajit_module() table function ===== */

typedef struct { char *mode, *source, *sql_name, *msg; bool ok; } bind_t;
typedef struct { bool done; } state_t;

static void tf_bind(duckdb_bind_info info) {
    bind_t *d = (bind_t *)duckdb_malloc(sizeof(bind_t));
    memset(d, 0, sizeof(*d)); d->mode = strdup("info");
    duckdb_value v;
    #define G(n,f) v=duckdb_bind_get_named_parameter(info,n); \
        if(v){free(d->f);d->f=strdup(duckdb_get_varchar(v));duckdb_destroy_value(&v);}
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

    if (luaL_loadstring(L, d->source) != LUA_OK)
        { d->msg = strdup(lua_tostring(L, -1)); lua_pop(L, 1); return; }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK)
        { d->msg = strdup(lua_tostring(L, -1)); lua_pop(L, 1); return; }
    if (!lua_isfunction(L, -1))
        { d->msg = strdup("must return a function"); lua_pop(L, 1); return; }
    int fref = luaL_ref(L, LUA_REGISTRYINDEX);

    /* Probe arity */
    int np = 0;
    lua_getglobal(L, "debug"); lua_getfield(L, -1, "getinfo");
    lua_rawgeti(L, LUA_REGISTRYINDEX, fref); lua_pushstring(L, "u");
    if (lua_pcall(L, 2, 1, 0) == LUA_OK && lua_istable(L, -1))
        { lua_getfield(L, -1, "nparams"); np = (int)lua_tointeger(L, -1); lua_pop(L, 1); }
    lua_pop(L, 2);

    if (!g_conn) { d->msg = strdup("no conn"); return; }

    /* Store in global UDF table */
    udf_put(d->sql_name, fref);

    duckdb_scalar_function u = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(u, d->sql_name);
    duckdb_scalar_function_set_function(u, f_udf);
    duckdb_scalar_function_set_return_type(u, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    for (int i = 0; i < np; i++)
        duckdb_scalar_function_add_parameter(u, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_register_scalar_function(g_conn, u);
    duckdb_destroy_scalar_function(&u);

    d->ok = true; d->msg = NULL;
}

static void tf_func(duckdb_function_info info, duckdb_data_chunk output) {
    bind_t *d = (bind_t *)duckdb_function_get_bind_data(info);
    state_t *s = (state_t *)duckdb_function_get_init_data(info);
    if (!d || !s || s->done) { duckdb_data_chunk_set_size(output, 0); return; }
    duckdb_data_chunk_set_size(output, 1);
    ((bool *)duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 0)))[0] = d->ok;
    duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(output, 1), 0, d->mode ? d->mode : "?");
    duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(output, 2), 0,
        d->msg ? d->msg : (d->ok ? "OK" : "ERROR"));
    s->done = true;
}

/* ===== Init ===== */

void luajit_register_module_functions(
    duckdb_connection temp_conn, duckdb_extension_info ei, struct duckdb_extension_access *acc)
{
    ensure_lua();
    duckdb_database *dbp = acc->get_database(ei);
    if (dbp && !g_conn) duckdb_connect(*dbp, &g_conn);

    duckdb_scalar_function f = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(f, "luajit");
    duckdb_scalar_function_set_function(f, f_luajit);
    duckdb_scalar_function_set_return_type(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_scalar_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_register_scalar_function(temp_conn, f);
    duckdb_destroy_scalar_function(&f);

    duckdb_table_function t = duckdb_create_table_function();
    duckdb_table_function_set_name(t, "luajit_module");
    duckdb_table_function_add_named_parameter(t, "mode", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_table_function_add_named_parameter(t, "source", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_table_function_add_named_parameter(t, "sql_name", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_table_function_set_bind(t, tf_bind);
    duckdb_table_function_set_init(t, tf_init);
    duckdb_table_function_set_function(t, tf_func);
    duckdb_register_table_function(temp_conn, t);
    duckdb_destroy_table_function(&t);
}
