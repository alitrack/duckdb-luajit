/*
 * luajit — DuckDB LuaJIT UDF Extension  v0.4
 * SPDX-License-Identifier: MIT
 *
 * - luajit(expr)          → VARCHAR  (inline eval)
 * - luajit_i(name, ...)   → BIGINT   (typed int UDF call)
 * - luajit_f(name, ...)   → DOUBLE   (typed float UDF call)
 * - luajit_module(compile) → store UDF as Lua global
 * - luajit_module(macro)   → generate CREATE MACRO DDL
 */

#include "duckdb_extension.h"

DUCKDB_EXTENSION_EXTERN

#ifdef LUAJIT_WASM_STUB
/* ── WASM stub: LuaJIT not available ── */
void luajit_register_module_functions(
    duckdb_connection conn, duckdb_extension_info ei, struct duckdb_extension_access *acc)
{ (void)conn; (void)ei; (void)acc; }
#else

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <string.h>
#include <stdlib.h>

static lua_State *g_lua = NULL;
static void L_(void) { if (!g_lua) { g_lua = luaL_newstate(); if (g_lua) luaL_openlibs(g_lua); } }

/* ── luajit(str) → VARCHAR ── */

static void f_luajit(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    L_(); lua_State *L = g_lua; if (!L) return;
    idx_t n = duckdb_data_chunk_get_size(in);
    for (idx_t r = 0; r < n; r++) {
        duckdb_vector sv = duckdb_data_chunk_get_vector(in, 0);
        duckdb_string_t ss = ((duckdb_string_t *)duckdb_vector_get_data(sv))[r];
        if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(sv), r))
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r); continue; }
        int ok = luaL_loadbuffer(L, duckdb_string_t_data(&ss), duckdb_string_t_length(ss), "lj");
        ok = ok || lua_pcall(L, 0, 1, 0);
        if (ok) { duckdb_vector_assign_string_element(out, r, lua_tostring(L, -1)); lua_pop(L, 1); continue; }
        if (lua_isnil(L, -1)) duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r);
        else { lua_getglobal(L, "tostring"); lua_pushvalue(L, -2); lua_pcall(L, 1, 1, 0);
               duckdb_vector_assign_string_element(out, r, lua_tostring(L, -1)); lua_pop(L, 1); }
        lua_pop(L, 1);
    }
}

/* ── luajit_i(name VARCHAR, ...BIGINT) → BIGINT ── */

static void f_luajit_i(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    L_(); lua_State *L = g_lua; if (!L) return;
    idx_t nr = duckdb_data_chunk_get_size(in);
    idx_t nc = duckdb_data_chunk_get_column_count(in);
    int64_t *od = (int64_t *)duckdb_vector_get_data(out);
    for (idx_t r = 0; r < nr; r++) {
        duckdb_vector nv = duckdb_data_chunk_get_vector(in, 0);
        duckdb_string_t ns = ((duckdb_string_t *)duckdb_vector_get_data(nv))[r];
        if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(nv), r))
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r); continue; }
        lua_getglobal(L, duckdb_string_t_data(&ns));
        if (!lua_isfunction(L, -1))
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r); lua_pop(L, 1); continue; }
        for (idx_t c = 1; c < nc; c++) {
            duckdb_vector v = duckdb_data_chunk_get_vector(in, c);
            int64_t *d = (int64_t *)duckdb_vector_get_data(v);
            if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(v), r))
                lua_pushnil(L);
            else
                lua_pushinteger(L, (lua_Integer)d[r]);
        }
        if (lua_pcall(L, (int)(nc - 1), 1, 0) != LUA_OK)
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r); lua_pop(L, 1); continue; }
        if (lua_isnil(L, -1) || !lua_isnumber(L, -1))
            duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r);
        else
            od[r] = (int64_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
}

/* ── luajit_f(name VARCHAR, ...DOUBLE) → DOUBLE ── */

static void f_luajit_f(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    L_(); lua_State *L = g_lua; if (!L) return;
    idx_t nr = duckdb_data_chunk_get_size(in);
    idx_t nc = duckdb_data_chunk_get_column_count(in);
    double *od = (double *)duckdb_vector_get_data(out);
    for (idx_t r = 0; r < nr; r++) {
        duckdb_vector nv = duckdb_data_chunk_get_vector(in, 0);
        duckdb_string_t ns = ((duckdb_string_t *)duckdb_vector_get_data(nv))[r];
        if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(nv), r))
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r); continue; }
        lua_getglobal(L, duckdb_string_t_data(&ns));
        if (!lua_isfunction(L, -1))
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r); lua_pop(L, 1); continue; }
        for (idx_t c = 1; c < nc; c++) {
            duckdb_vector v = duckdb_data_chunk_get_vector(in, c);
            double *d = (double *)duckdb_vector_get_data(v);
            if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(v), r))
                lua_pushnil(L);
            else
                lua_pushnumber(L, d[r]);
        }
        if (lua_pcall(L, (int)(nc - 1), 1, 0) != LUA_OK)
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r); lua_pop(L, 1); continue; }
        if (lua_isnil(L, -1) || !lua_isnumber(L, -1))
            duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r);
        else
            od[r] = lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
}

/* ── luajit_module() table function ── */

typedef struct { char *mode, *source, *sql_name, *msg; bool ok; } mod_bind_t;
typedef struct { bool done; } mod_state_t;

static void mod_bind(duckdb_bind_info info) {
    mod_bind_t *d = (mod_bind_t *)duckdb_malloc(sizeof(mod_bind_t));
    memset(d, 0, sizeof(*d)); d->mode = strdup("info");
    #define G(n, f) do { \
        duckdb_value v = duckdb_bind_get_named_parameter(info, n); \
        if (v) { free(d->f); d->f = strdup(duckdb_get_varchar(v)); duckdb_destroy_value(&v); } \
    } while (0)
    G("mode", mode); G("source", source); G("sql_name", sql_name);
    #undef G
    duckdb_bind_add_result_column(info, "ok", duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN));
    duckdb_bind_add_result_column(info, "mode", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_bind_add_result_column(info, "message", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_bind_set_bind_data(info, d, free);
}

static void mod_init(duckdb_init_info info) {
    mod_bind_t *d = (mod_bind_t *)duckdb_init_get_bind_data(info);
    if (!d) return;
    mod_state_t *s = (mod_state_t *)duckdb_malloc(sizeof(mod_state_t));
    memset(s, 0, sizeof(*s));
    duckdb_init_set_init_data(info, s, free);

    if (strcmp(d->mode, "info") == 0) { d->ok = true; return; }
    if (strcmp(d->mode, "compile") != 0 && strcmp(d->mode, "macro") != 0)
        { d->msg = strdup("bad mode"); return; }

    /* ── macro mode: generate CREATE MACRO DDL ── */
    if (strcmp(d->mode, "macro") == 0) {
        if (!d->sql_name) { d->msg = strdup("need sql_name for macro"); return; }
        const char *var = "v";
        if (d->source) {
            if (strcmp(d->source, "i") == 0 || strcmp(d->source, "BIGINT") == 0) var = "i";
            else if (strcmp(d->source, "f") == 0 || strcmp(d->source, "DOUBLE") == 0) var = "f";
        }
        const char *fn = (*var == 'i') ? "luajit_i" : (*var == 'f') ? "luajit_f" : "luajit";
        char buf[4096];
        int off = snprintf(buf, sizeof(buf), "CREATE OR REPLACE MACRO %s(", d->sql_name);
        int nargs = 2;
        L_(); lua_State *L = g_lua;
        lua_getglobal(L, d->sql_name);
        if (lua_isfunction(L, -1)) {
            lua_getglobal(L, "debug"); lua_getfield(L, -1, "getinfo");
            lua_pushvalue(L, -3); lua_pushstring(L, "u");
            if (lua_pcall(L, 2, 1, 0) == LUA_OK && lua_istable(L, -1))
                { lua_getfield(L, -1, "nparams"); nargs = (int)lua_tointeger(L, -1); lua_pop(L, 1); }
            lua_pop(L, 2);
        }
        lua_pop(L, 1);
        for (int i = 0; i < nargs; i++)
            off += snprintf(buf + off, sizeof(buf) - off, "%sx%d", i > 0 ? ", " : "", i + 1);
        off += snprintf(buf + off, sizeof(buf) - off, ") AS %s('", fn);
        off += snprintf(buf + off, sizeof(buf) - off, "%s'", d->sql_name);
        for (int i = 0; i < nargs; i++)
            off += snprintf(buf + off, sizeof(buf) - off, ", x%d", i + 1);
        snprintf(buf + off, sizeof(buf) - off, ")");
        d->ok = true; d->msg = strdup(buf);
        return;
    }

    /* ── compile mode ── */
    if (!d->source || !d->sql_name) { d->msg = strdup("need source+sql_name"); return; }

    L_(); lua_State *L = g_lua;
    if (!L) { d->msg = strdup("no Lua"); return; }

    if (luaL_loadstring(L, d->source) != LUA_OK)
        { d->msg = strdup(lua_tostring(L, -1)); lua_pop(L, 1); return; }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK)
        { d->msg = strdup(lua_tostring(L, -1)); lua_pop(L, 1); return; }
    if (!lua_isfunction(L, -1))
        { d->msg = strdup("must return a function"); lua_pop(L, 1); return; }

    lua_setglobal(L, d->sql_name);
    d->ok = true; d->msg = NULL;
}

static void mod_func(duckdb_function_info fi, duckdb_data_chunk out) {
    mod_bind_t *d = (mod_bind_t *)duckdb_function_get_bind_data(fi);
    mod_state_t *s = (mod_state_t *)duckdb_function_get_init_data(fi);
    if (!d || !s || s->done) { duckdb_data_chunk_set_size(out, 0); return; }
    duckdb_data_chunk_set_size(out, 1);
    ((bool *)duckdb_vector_get_data(duckdb_data_chunk_get_vector(out, 0)))[0] = d->ok;
    duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(out, 1), 0, d->mode ? d->mode : "?");
    duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(out, 2), 0,
        d->msg ? d->msg : (d->ok ? "OK" : "ERROR"));
    s->done = true;
}

/* ── Registration ── */

void luajit_register_module_functions(
    duckdb_connection conn, duckdb_extension_info ei, struct duckdb_extension_access *acc)
{
    (void)ei; (void)acc;
    L_();

    { duckdb_scalar_function f = duckdb_create_scalar_function();
      duckdb_scalar_function_set_name(f, "luajit");
      duckdb_scalar_function_set_function(f, f_luajit);
      duckdb_scalar_function_set_return_type(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_scalar_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_register_scalar_function(conn, f); duckdb_destroy_scalar_function(&f); }

    { duckdb_scalar_function f = duckdb_create_scalar_function();
      duckdb_scalar_function_set_name(f, "luajit_i");
      duckdb_scalar_function_set_function(f, f_luajit_i);
      duckdb_scalar_function_set_return_type(f, duckdb_create_logical_type(DUCKDB_TYPE_BIGINT));
      duckdb_scalar_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_scalar_function_set_varargs(f, duckdb_create_logical_type(DUCKDB_TYPE_BIGINT));
      duckdb_register_scalar_function(conn, f); duckdb_destroy_scalar_function(&f); }

    { duckdb_scalar_function f = duckdb_create_scalar_function();
      duckdb_scalar_function_set_name(f, "luajit_f");
      duckdb_scalar_function_set_function(f, f_luajit_f);
      duckdb_scalar_function_set_return_type(f, duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE));
      duckdb_scalar_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_scalar_function_set_varargs(f, duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE));
      duckdb_register_scalar_function(conn, f); duckdb_destroy_scalar_function(&f); }

    { duckdb_table_function t = duckdb_create_table_function();
      duckdb_table_function_set_name(t, "luajit_module");
      duckdb_table_function_add_named_parameter(t, "mode", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_table_function_add_named_parameter(t, "source", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_table_function_add_named_parameter(t, "sql_name", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_table_function_set_bind(t, mod_bind);
      duckdb_table_function_set_init(t, mod_init);
      duckdb_table_function_set_function(t, mod_func);
      duckdb_register_table_function(conn, t); duckdb_destroy_table_function(&t); }
}

#endif /* !LUAJIT_WASM_STUB */
