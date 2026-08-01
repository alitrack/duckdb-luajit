/*
 * luajit — DuckDB LuaJIT UDF Extension  v0.5
 * SPDX-License-Identifier: MIT
 *
 * Full API:
 *   luajit(expr)           → VARCHAR   inline eval
 *   luajit_i(name, ...)    → BIGINT    typed int UDF
 *   luajit_f(name, ...)    → DOUBLE    typed float UDF
 *   luajit_b(name, ...)    → BOOLEAN   typed bool UDF
 *   luajit_m(name, ...)    → DOUBLE    mixed-type (auto-cast to double)
 *   luajit_module(modes):  info / compile / macro / list / drop / reset
 */

#include "duckdb_extension.h"

DUCKDB_EXTENSION_EXTERN

#ifdef LUAJIT_WASM_STUB
void luajit_register_module_functions(
    duckdb_connection c, duckdb_extension_info e, struct duckdb_extension_access *a)
{ (void)c; (void)e; (void)a; }
#else

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* ── Thread safety: single global Lua state, guarded by a recursive mutex ──
 * DuckDB executes UDFs in parallel across threads; every Lua access must be
 * serialized. Recursive so _duckdb_call → duckdb_query → luajit UDF can re-enter.
 */
#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_lua_lock;
static void lua_lock_init(void) { InitializeCriticalSection(&g_lua_lock); }
#define LUA_LOCK()   EnterCriticalSection(&g_lua_lock)
#define LUA_UNLOCK() LeaveCriticalSection(&g_lua_lock)
#else
#include <pthread.h>
static pthread_mutex_t g_lua_lock;
static void lua_lock_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lua_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}
#define LUA_LOCK()   pthread_mutex_lock(&g_lua_lock)
#define LUA_UNLOCK() pthread_mutex_unlock(&g_lua_lock)
#endif

/* ── Thread-safety guard for executor bodies ──
 * Usage:
 *   static void fn(...) {
 *       LUA_BEGIN();              // locks, ensures g_lua, goto lua_cleanup if NULL
 *       ...                       // every early return becomes: goto lua_cleanup;
 *       LUA_CLEANUP();            // label + unlock
 *   }
 */
#define LUA_BEGIN()  LUA_LOCK(); L_(); lua_State *L = g_lua; if (!L) goto lua_cleanup;
#define LUA_CLEANUP() lua_cleanup: LUA_UNLOCK()

static lua_State *g_lua = NULL;
static duckdb_connection g_conn = NULL;

/* ── DuckDB callback from Lua: _duckdb_call(sql) → status VARCHAR ── */
static int l_duckdb_call(lua_State *L) {
    if (!g_conn) { lua_pushstring(L, "no connection"); return 1; }
    const char *sql = luaL_checkstring(L, 1);
    duckdb_result res;
    if (duckdb_query(g_conn, sql, &res) != DuckDBSuccess) {
        lua_pushfstring(L, "error: %s", duckdb_result_error(&res));
        duckdb_destroy_result(&res); return 1;
    }
    lua_pushstring(L, "ok");
    duckdb_destroy_result(&res);
    return 1;
}

static void L_(void) {
    if (!g_lua) {
        g_lua = luaL_newstate();
        if (g_lua) {
            luaL_openlibs(g_lua);
            /* register DuckDB callback */
            lua_pushcfunction(g_lua, l_duckdb_call);
            lua_setglobal(g_lua, "_duckdb_call");
        }
    }
}

/* ── helpers ── */

static void push_str(lua_State *L, duckdb_vector v, idx_t r) {
    duckdb_string_t s=((duckdb_string_t*)duckdb_vector_get_data(v))[r];
    if(!duckdb_validity_row_is_valid(duckdb_vector_get_validity(v),r))
        lua_pushnil(L);
    else lua_pushlstring(L,duckdb_string_t_data(&s),duckdb_string_t_length(s));
}
static void push_int(lua_State *L, duckdb_vector v, idx_t r) {
    int64_t*d=(int64_t*)duckdb_vector_get_data(v);
    if(!duckdb_validity_row_is_valid(duckdb_vector_get_validity(v),r))
        lua_pushnil(L);
    else lua_pushinteger(L,(lua_Integer)d[r]);
}
static void push_flt(lua_State *L, duckdb_vector v, idx_t r) {
    double*d=(double*)duckdb_vector_get_data(v);
    if(!duckdb_validity_row_is_valid(duckdb_vector_get_validity(v),r))
        lua_pushnil(L);
    else lua_pushnumber(L,d[r]);
}
static void push_bool(lua_State *L, duckdb_vector v, idx_t r) {
    bool*d=(bool*)duckdb_vector_get_data(v);
    if(!duckdb_validity_row_is_valid(duckdb_vector_get_validity(v),r))
        lua_pushnil(L);
    else
        lua_pushinteger(L, d[r] ? 1 : 0); /* Lua boolean can't do arithmetic */
}
static void to_str(lua_State *L) {
    if(lua_isnil(L,-1))return;if(lua_isstring(L,-1))return;
    lua_getglobal(L,"tostring");lua_pushvalue(L,-2);lua_pcall(L,1,1,0);
}

/* ── luajit(str) → VARCHAR ── */

static void fj(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t n=duckdb_data_chunk_get_size(in);
    for(idx_t r=0;r<n;r++){
        duckdb_vector sv=duckdb_data_chunk_get_vector(in,0);
        duckdb_string_t ss=((duckdb_string_t*)duckdb_vector_get_data(sv))[r];
        if(!duckdb_validity_row_is_valid(duckdb_vector_get_validity(sv),r))
            {duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);continue;}
        int ok=luaL_loadbuffer(L,duckdb_string_t_data(&ss),duckdb_string_t_length(ss),"lj");
        ok=ok||lua_pcall(L,0,1,0);
        if(ok){duckdb_vector_assign_string_element(out,r,lua_tostring(L,-1));lua_pop(L,1);continue;}
        if(lua_isnil(L,-1))duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);
        else{to_str(L);duckdb_vector_assign_string_element(out,r,lua_tostring(L,-1));lua_pop(L,1);}
        lua_pop(L,1);
    }
    LUA_CLEANUP();
}

/* ── UDF executors ── */

static int resolve_udf(lua_State *L, duckdb_vector nv, idx_t r) {
    duckdb_string_t ns=((duckdb_string_t*)duckdb_vector_get_data(nv))[r];
    if(!duckdb_validity_row_is_valid(duckdb_vector_get_validity(nv),r)) return 0;
    lua_getglobal(L,duckdb_string_t_data(&ns));
    int ok=lua_isfunction(L,-1);
    if(!ok)lua_pop(L,1);
    return ok;
}

/* luajit_i(name, ...BIGINT) → BIGINT */
static void fji(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr=duckdb_data_chunk_get_size(in),nc=duckdb_data_chunk_get_column_count(in);
    int64_t*od=(int64_t*)duckdb_vector_get_data(out);
    for(idx_t r=0;r<nr;r++){
        if(!resolve_udf(L,duckdb_data_chunk_get_vector(in,0),r))
            {duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);continue;}
        for(idx_t c=1;c<nc;c++)push_int(L,duckdb_data_chunk_get_vector(in,c),r);
        if(lua_pcall(L,(int)(nc-1),1,0)!=LUA_OK)
            {duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);lua_pop(L,1);continue;}
        if(lua_isnil(L,-1)||!lua_isnumber(L,-1))
            duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);
        else od[r]=(int64_t)lua_tointeger(L,-1);
        lua_pop(L,1);
    }
    LUA_CLEANUP();
}

/* luajit_f(name, ...DOUBLE) → DOUBLE */
static void fjf(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr=duckdb_data_chunk_get_size(in),nc=duckdb_data_chunk_get_column_count(in);
    double*od=(double*)duckdb_vector_get_data(out);
    for(idx_t r=0;r<nr;r++){
        if(!resolve_udf(L,duckdb_data_chunk_get_vector(in,0),r))
            {duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);continue;}
        for(idx_t c=1;c<nc;c++)push_flt(L,duckdb_data_chunk_get_vector(in,c),r);
        if(lua_pcall(L,(int)(nc-1),1,0)!=LUA_OK)
            {duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);lua_pop(L,1);continue;}
        if(lua_isnil(L,-1)||!lua_isnumber(L,-1))
            duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);
        else od[r]=lua_tonumber(L,-1);
        lua_pop(L,1);
    }
    LUA_CLEANUP();
}

/* luajit_v(name, ...DOUBLE) → DOUBLE  — chunk-batched: 1 Lua call per chunk */
static void fjv(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr = duckdb_data_chunk_get_size(in);
    idx_t nc = duckdb_data_chunk_get_column_count(in);
    if (nr == 0 || nc < 2) goto lua_cleanup;

    double *od = (double *)duckdb_vector_get_data(out);

    /* Resolve UDF name (from first row's column 0) */
    if (!resolve_udf(L, duckdb_data_chunk_get_vector(in, 0), 0)) goto lua_cleanup;
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); goto lua_cleanup; }

    /* Build one Lua table per arg column: {val_1, val_2, ..., val_nr} */
    int nargs = (int)(nc - 1);
    for (int c = 1; c < (int)nc; c++) {
        lua_createtable(L, (int)nr, 0);
        duckdb_vector v = duckdb_data_chunk_get_vector(in, (idx_t)c);
        double *d = (double *)duckdb_vector_get_data(v);
        for (idx_t r = 0; r < nr; r++) {
            lua_pushnumber(L, d[r]);
            lua_rawseti(L, -2, (int)(r + 1));
        }
    }

    /* Call UDF(t1, t2, ...) once per chunk — func already at bottom */
    if (lua_pcall(L, nargs, 1, 0) != LUA_OK || !lua_istable(L, -1)) {
        lua_pop(L, lua_gettop(L)); /* clear stack */
        goto lua_cleanup;
    }

    /* Unpack result table → output vector */
    idx_t rlen = (idx_t)lua_objlen(L, -1);
    for (idx_t r = 0; r < nr && r < rlen; r++) {
        lua_rawgeti(L, -1, (int)(r + 1));
        od[r] = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    LUA_CLEANUP();
}

/* luajit_b(name, ...BOOLEAN) → BOOLEAN */
static void fjb(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr=duckdb_data_chunk_get_size(in),nc=duckdb_data_chunk_get_column_count(in);
    bool*od=(bool*)duckdb_vector_get_data(out);
    for(idx_t r=0;r<nr;r++){
        if(!resolve_udf(L,duckdb_data_chunk_get_vector(in,0),r))
            {duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);continue;}
        for(idx_t c=1;c<nc;c++)push_bool(L,duckdb_data_chunk_get_vector(in,c),r);
        if(lua_pcall(L,(int)(nc-1),1,0)!=LUA_OK)
            {duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);lua_pop(L,1);continue;}
        if(lua_isnil(L,-1))duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);
        else if(lua_isboolean(L,-1))od[r]=lua_toboolean(L,-1)?true:false;
        else if(lua_isnumber(L,-1))od[r]=lua_tonumber(L,-1)!=0.0;
        else od[r]=lua_toboolean(L,-1)?true:false;
        lua_pop(L,1);
    }
    LUA_CLEANUP();
}

/* luajit_m(name, ...DOUBLE) → DOUBLE  (mixed-type: all args auto-cast to double) */
static void fjm(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr=duckdb_data_chunk_get_size(in),nc=duckdb_data_chunk_get_column_count(in);
    double*od=(double*)duckdb_vector_get_data(out);
    for(idx_t r=0;r<nr;r++){
        if(!resolve_udf(L,duckdb_data_chunk_get_vector(in,0),r))
            {duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);continue;}
        /* auto-cast: try as int, then as double */
        for(idx_t c=1;c<nc;c++){
            duckdb_vector v=duckdb_data_chunk_get_vector(in,c);
            duckdb_type dt=duckdb_get_type_id(duckdb_vector_get_column_type(v));
            if(dt==DUCKDB_TYPE_BIGINT||dt==DUCKDB_TYPE_INTEGER||dt==DUCKDB_TYPE_SMALLINT||dt==DUCKDB_TYPE_TINYINT)
                push_int(L,v,r);
            else if(dt==DUCKDB_TYPE_DOUBLE||dt==DUCKDB_TYPE_FLOAT)
                push_flt(L,v,r);
            else if(dt==DUCKDB_TYPE_BOOLEAN)
                push_bool(L,v,r);
            else
                push_str(L,v,r);
        }
        if(lua_pcall(L,(int)(nc-1),1,0)!=LUA_OK)
            {duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);lua_pop(L,1);continue;}
        if(lua_isnil(L,-1)||!lua_isnumber(L,-1))
            duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out),r);
        else od[r]=lua_tonumber(L,-1);
        lua_pop(L,1);
    }
    LUA_CLEANUP();
}

/* ── LIST bridge ── */

typedef struct { uint64_t offset; uint64_t length; } list_entry_t;

static void push_list_to_lua(lua_State *L, duckdb_vector list_vec, duckdb_vector child_vec, idx_t r) {
    list_entry_t *entries = (list_entry_t *)duckdb_vector_get_data(list_vec);
    uint64_t off = entries[r].offset, len = entries[r].length;
    duckdb_type ct = duckdb_get_type_id(duckdb_vector_get_column_type(child_vec));
    lua_createtable(L, (int)len, 0);
    if (ct == DUCKDB_TYPE_BIGINT || ct == DUCKDB_TYPE_INTEGER) {
        int64_t *d = (int64_t *)duckdb_vector_get_data(child_vec);
        for (uint64_t i = 0; i < len; i++)
            { lua_pushinteger(L, (lua_Integer)d[off + i]); lua_rawseti(L, -2, (int)(i + 1)); }
    } else if (ct == DUCKDB_TYPE_DOUBLE || ct == DUCKDB_TYPE_FLOAT) {
        double *d = (double *)duckdb_vector_get_data(child_vec);
        for (uint64_t i = 0; i < len; i++)
            { lua_pushnumber(L, d[off + i]); lua_rawseti(L, -2, (int)(i + 1)); }
    } else if (ct == DUCKDB_TYPE_VARCHAR) {
        duckdb_string_t *d = (duckdb_string_t *)duckdb_vector_get_data(child_vec);
        for (uint64_t i = 0; i < len; i++) {
            duckdb_string_t s = d[off + i];
            lua_pushlstring(L, duckdb_string_t_data(&s), duckdb_string_t_length(s));
            lua_rawseti(L, -2, (int)(i + 1));
        }
    }
}

static void write_lua_to_list(lua_State *L, duckdb_vector out, duckdb_vector child, idx_t row) {
    int n = (int)lua_objlen(L, -1); if (n == 0) return;
    duckdb_type ct = duckdb_get_type_id(duckdb_vector_get_column_type(child));
    idx_t cs = duckdb_list_vector_get_size(out);
    duckdb_list_vector_reserve(out, cs + n);
    list_entry_t *e = (list_entry_t *)duckdb_vector_get_data(out);
    e[row].offset = cs; e[row].length = n;
    duckdb_list_vector_set_size(out, cs + n);
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, -1, i + 1); idx_t p = cs + i;
        if (ct == DUCKDB_TYPE_DOUBLE || ct == DUCKDB_TYPE_FLOAT)
            ((double *)duckdb_vector_get_data(child))[p] = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
        else if (ct == DUCKDB_TYPE_BIGINT || ct == DUCKDB_TYPE_INTEGER)
            ((int64_t *)duckdb_vector_get_data(child))[p] = lua_isnumber(L, -1) ? (int64_t)lua_tointeger(L, -1) : 0;
        else if (ct == DUCKDB_TYPE_VARCHAR)
            duckdb_vector_assign_string_element(child, p, lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
        lua_pop(L, 1);
    }
}

static void fjl(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr = duckdb_data_chunk_get_size(in), nc = duckdb_data_chunk_get_column_count(in);
    duckdb_vector oc = duckdb_list_vector_get_child(out);
    for (idx_t r = 0; r < nr; r++) {
        if (!resolve_udf(L, duckdb_data_chunk_get_vector(in, 0), r))
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r); continue; }
        for (idx_t c = 1; c < nc; c++)
            push_list_to_lua(L, duckdb_data_chunk_get_vector(in, c),
                             duckdb_list_vector_get_child(duckdb_data_chunk_get_vector(in, c)), r);
        if (lua_pcall(L, (int)(nc - 1), 1, 0) != LUA_OK)
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r); lua_pop(L, 1); continue; }
        if (!lua_istable(L, -1))
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r); lua_pop(L, 1); continue; }
        write_lua_to_list(L, out, oc, r);
        lua_pop(L, 1);
    }
    LUA_CLEANUP();
}

/* ── STRUCT bridge: DuckDB STRUCT ↔ Lua table (named keys) ── */

static void push_struct_to_lua(lua_State *L, duckdb_vector sv, idx_t r) {
    duckdb_logical_type st = duckdb_vector_get_column_type(sv);
    idx_t nc = duckdb_struct_type_child_count(st);
    lua_createtable(L, 0, (int)nc);
    for (idx_t i = 0; i < nc; i++) {
        char *name = duckdb_struct_type_child_name(st, i);
        duckdb_vector child = duckdb_struct_vector_get_child(sv, i);
        duckdb_type ct = duckdb_get_type_id(duckdb_struct_type_child_type(st, i));
        if (ct == DUCKDB_TYPE_BIGINT || ct == DUCKDB_TYPE_INTEGER || ct == DUCKDB_TYPE_SMALLINT)
            { lua_pushinteger(L, ((int64_t*)duckdb_vector_get_data(child))[r]);
              lua_setfield(L, -2, name); }
        else if (ct == DUCKDB_TYPE_DOUBLE || ct == DUCKDB_TYPE_FLOAT)
            { lua_pushnumber(L, ((double*)duckdb_vector_get_data(child))[r]);
              lua_setfield(L, -2, name); }
        else if (ct == DUCKDB_TYPE_VARCHAR) {
            duckdb_string_t s = ((duckdb_string_t*)duckdb_vector_get_data(child))[r];
            lua_pushlstring(L, duckdb_string_t_data(&s), duckdb_string_t_length(s));
            lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_BOOLEAN)
            { lua_pushboolean(L, ((bool*)duckdb_vector_get_data(child))[r]);
              lua_setfield(L, -2, name); }
        duckdb_free(name);
    }
}

static void write_lua_to_struct(lua_State *L, duckdb_vector out, idx_t row) {
    duckdb_logical_type st = duckdb_vector_get_column_type(out);
    idx_t nc = duckdb_struct_type_child_count(st);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        const char *key = lua_tostring(L, -2);
        if (!key) { lua_pop(L, 1); continue; }
        for (idx_t i = 0; i < nc; i++) {
            char *name = duckdb_struct_type_child_name(st, i);
            if (!strcmp(key, name)) {
                duckdb_vector child = duckdb_struct_vector_get_child(out, i);
                duckdb_type ct = duckdb_get_type_id(duckdb_struct_type_child_type(st, i));
                if (ct == DUCKDB_TYPE_BIGINT || ct == DUCKDB_TYPE_INTEGER)
                    ((int64_t*)duckdb_vector_get_data(child))[row] =
                        lua_isnumber(L, -1) ? (int64_t)lua_tointeger(L, -1) : 0;
                else if (ct == DUCKDB_TYPE_DOUBLE)
                    ((double*)duckdb_vector_get_data(child))[row] =
                        lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
                else if (ct == DUCKDB_TYPE_VARCHAR)
                    duckdb_vector_assign_string_element(child, row,
                        lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
                else if (ct == DUCKDB_TYPE_BOOLEAN)
                    ((bool*)duckdb_vector_get_data(child))[row] =
                        lua_toboolean(L, -1) ? true : false;
                duckdb_free(name);
                break;
            }
            duckdb_free(name);
        }
        lua_pop(L, 1);
    }
}

static void fjs(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr = duckdb_data_chunk_get_size(in);
    for (idx_t r = 0; r < nr; r++) {
        if (!resolve_udf(L, duckdb_data_chunk_get_vector(in, 0), r))
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r); continue; }
        push_struct_to_lua(L, duckdb_data_chunk_get_vector(in, 1), r);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK)
            { duckdb_vector_assign_string_element(out, r, lua_tostring(L, -1)); lua_pop(L, 1); continue; }
        to_str(L);
        duckdb_vector_assign_string_element(out, r, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    LUA_CLEANUP();
}

/* ── MAP bridge: DuckDB MAP ↔ Lua table (key→value) ── */

static void push_map_to_lua(lua_State *L, duckdb_vector mv, idx_t r) {
    list_entry_t *entries = (list_entry_t *)duckdb_vector_get_data(mv);
    uint64_t off = entries[r].offset, len = entries[r].length;
    duckdb_vector child = duckdb_list_vector_get_child(mv); /* STRUCT(key,value) */
    duckdb_vector key_vec = duckdb_struct_vector_get_child(child, 0);
    duckdb_vector val_vec = duckdb_struct_vector_get_child(child, 1);
    duckdb_type kt = duckdb_get_type_id(duckdb_vector_get_column_type(key_vec));
    duckdb_type vt = duckdb_get_type_id(duckdb_vector_get_column_type(val_vec));
    lua_createtable(L, 0, (int)len);
    for (uint64_t i = 0; i < len; i++) {
        /* push key */
        if (kt == DUCKDB_TYPE_BIGINT)
            lua_pushinteger(L, ((int64_t*)duckdb_vector_get_data(key_vec))[off + i]);
        else if (kt == DUCKDB_TYPE_INTEGER || kt == DUCKDB_TYPE_SMALLINT || kt == DUCKDB_TYPE_TINYINT)
            lua_pushinteger(L, ((int32_t*)duckdb_vector_get_data(key_vec))[off + i]);
        else if (kt == DUCKDB_TYPE_VARCHAR) {
            duckdb_string_t s = ((duckdb_string_t*)duckdb_vector_get_data(key_vec))[off + i];
            lua_pushlstring(L, duckdb_string_t_data(&s), duckdb_string_t_length(s));
        } else continue;
        /* push value */
        if (vt == DUCKDB_TYPE_DOUBLE || vt == DUCKDB_TYPE_FLOAT)
            lua_pushnumber(L, ((double*)duckdb_vector_get_data(val_vec))[off + i]);
        else if (vt == DUCKDB_TYPE_BIGINT)
            lua_pushinteger(L, ((int64_t*)duckdb_vector_get_data(val_vec))[off + i]);
        else if (vt == DUCKDB_TYPE_INTEGER || vt == DUCKDB_TYPE_SMALLINT || vt == DUCKDB_TYPE_TINYINT)
            lua_pushinteger(L, ((int32_t*)duckdb_vector_get_data(val_vec))[off + i]);
        else if (vt == DUCKDB_TYPE_VARCHAR) {
            duckdb_string_t s = ((duckdb_string_t*)duckdb_vector_get_data(val_vec))[off + i];
            lua_pushlstring(L, duckdb_string_t_data(&s), duckdb_string_t_length(s));
        } else { lua_pop(L, 1); continue; }
        lua_rawset(L, -3);
    }
}

static void fjm_map(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr = duckdb_data_chunk_get_size(in);
    for (idx_t r = 0; r < nr; r++) {
        if (!resolve_udf(L, duckdb_data_chunk_get_vector(in, 0), r))
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r); continue; }
        push_map_to_lua(L, duckdb_data_chunk_get_vector(in, 1), r);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK)
            { duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), r); lua_pop(L, 1); continue; }
        to_str(L);
        duckdb_vector_assign_string_element(out, r, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    LUA_CLEANUP();
}

/* ── Aggregate UDF: luajit_agg(name, arg) → DOUBLE ── */

/* Shared load-from-file helper (used by both mode='load' and auto-load) */
static int load_udfs_from_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    LUA_LOCK();
    L_(); lua_State *L = g_lua;
    if (!L) { fclose(fp); LUA_UNLOCK(); return 0; }

    int loaded = 0;
    char line[16384];
    while (fgets(line, sizeof(line), fp)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *name = line, *source = tab + 1;
        size_t sl = strlen(source);
        while (sl > 0 && (source[sl-1] == '\n' || source[sl-1] == '\r')) source[--sl] = 0;
        if (sl == 0 || !strcmp(source, "?")) continue;
        if (luaL_loadstring(L, source) != LUA_OK) { lua_pop(L, 1); continue; }
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) { lua_pop(L, 1); continue; }
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
        lua_setglobal(L, name);
        loaded++;
    }
    fclose(fp);
    LUA_UNLOCK();
    return loaded;
}

/* Per-state aggregate: values live inside DuckDB's aggregate state (a
 * pointer-sized slot), so parallel threads each own their own group and
 * agg_combine merges them — no global state, thread-safe by construction. */
typedef struct {
    char    *udf;       /* UDF name (strdup'd on first row) */
    double  *vals; idx_t len, cap;
} agg_group_t;

static idx_t agg_state_size(duckdb_function_info fi) { (void)fi; return sizeof(agg_group_t *); }

static void agg_init(duckdb_function_info fi, duckdb_aggregate_state st) {
    (void)fi;
    agg_group_t *g = (agg_group_t *)malloc(sizeof(agg_group_t));
    memset(g, 0, sizeof(*g));
    *(agg_group_t **)st = g;
}
static void agg_update(duckdb_function_info fi, duckdb_data_chunk in, duckdb_aggregate_state *states) {
    (void)fi;
    duckdb_vector nv = duckdb_data_chunk_get_vector(in, 0);
    duckdb_string_t ns0 = ((duckdb_string_t *)duckdb_vector_get_data(nv))[0];
    const char *udf_name = duckdb_string_t_data(&ns0);
    idx_t udf_len = duckdb_string_t_length(ns0);

    duckdb_vector cv = duckdb_data_chunk_get_vector(in, 1);
    double *cd = (double *)duckdb_vector_get_data(cv);
    uint64_t *validity = duckdb_vector_get_validity(cv);
    idx_t chunk_nr = duckdb_data_chunk_get_size(in);

    for (idx_t i = 0; i < chunk_nr; i++) {
        if (validity && !(validity[i / 64] & (1ULL << (i % 64)))) continue;
        agg_group_t *g = *(agg_group_t **)states[i];
        if (!g) continue;
        if (!g->udf) {
            g->udf = (char *)malloc(udf_len + 1);
            memcpy(g->udf, udf_name, udf_len);
            g->udf[udf_len] = 0;
        }
        if (g->len + 1 > g->cap) {
            g->cap = g->cap ? g->cap * 2 : 1024;
            g->vals = (double *)realloc(g->vals, g->cap * sizeof(double));
        }
        g->vals[g->len++] = cd[i];
    }
}

/* Merge source state into destination, then free the source group. */
static void agg_combine(duckdb_function_info fi, duckdb_aggregate_state *s, duckdb_aggregate_state *d, idx_t c) {
    (void)fi;
    for (idx_t i = 0; i < c; i++) {
        agg_group_t *sg = s[i] ? *(agg_group_t **)s[i] : NULL;
        agg_group_t *dg = d[i] ? *(agg_group_t **)d[i] : NULL;
        if (!sg) continue;
        if (!dg) { *(agg_group_t **)d[i] = sg; *(agg_group_t **)s[i] = NULL; continue; }
        if (!dg->udf && sg->udf) { dg->udf = sg->udf; sg->udf = NULL; }
        for (idx_t j = 0; j < sg->len; j++) {
            if (dg->len + 1 > dg->cap) {
                dg->cap = dg->cap ? dg->cap * 2 : 1024;
                dg->vals = (double *)realloc(dg->vals, dg->cap * sizeof(double));
            }
            dg->vals[dg->len++] = sg->vals[j];
        }
        free(sg->vals);
        free(sg->udf);
        free(sg);
        *(agg_group_t **)s[i] = NULL;
    }
}

static void agg_destroy(duckdb_function_info fi, duckdb_aggregate_state *states, idx_t count) {
    (void)fi;
    for (idx_t i = 0; i < count; i++) {
        agg_group_t *g = *(agg_group_t **)states[i];
        if (g) { free(g->vals); free(g->udf); free(g); }
        *(agg_group_t **)states[i] = NULL;
    }
}

static void agg_finalize(duckdb_function_info fi, duckdb_aggregate_state *src, duckdb_vector result, idx_t count, idx_t offset) {
    LUA_LOCK();

    double *od = (double *)duckdb_vector_get_data(result);

    L_(); lua_State *L = g_lua;
    if (!L) { LUA_UNLOCK(); return; }

    int udf_ref = LUA_NOREF;
    const char *udf_name = NULL;

    for (idx_t i = 0; i < count; i++) {
        agg_group_t *g = *(agg_group_t **)src[i];
        if (!g || !g->vals || g->len == 0 || !g->udf) { od[offset + i] = 0.0; continue; }

        /* (Re)resolve the Lua UDF when the name changes across states */
        if (!udf_name || strcmp(udf_name, g->udf)) {
            if (udf_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, udf_ref);
            udf_name = g->udf;
            lua_getglobal(L, udf_name);
            if (!lua_isfunction(L, -1)) { lua_pop(L, 1); udf_ref = LUA_NOREF; od[offset + i] = 0.0; continue; }
            udf_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* keep UDF alive across calls */
        }

        lua_rawgeti(L, LUA_REGISTRYINDEX, udf_ref);
        lua_createtable(L, (int)g->len, 0);
        for (idx_t j = 0; j < g->len; j++)
            { lua_pushnumber(L, g->vals[j]); lua_rawseti(L, -2, (int)(j + 1)); }

        if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isnumber(L, -1))
            od[offset + i] = lua_tonumber(L, -1);
        else
            od[offset + i] = 0.0;
        lua_pop(L, 1);
    }
    if (udf_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, udf_ref);
    LUA_UNLOCK();
}

/* ── luajit_table: Lua table function ── */

typedef struct { char *source; } tbt_data_t;
typedef struct { int cur_row; char **rows; int nrows; } tbt_state_t;

static void tbt_bind(duckdb_bind_info info) {
    tbt_data_t *d = (tbt_data_t *)duckdb_malloc(sizeof(tbt_data_t));
    memset(d, 0, sizeof(*d));
    duckdb_value v = duckdb_bind_get_parameter(info, 0);
    if (v) { d->source = strdup(duckdb_get_varchar(v)); duckdb_destroy_value(&v); }
    duckdb_bind_add_result_column(info, "row_idx", duckdb_create_logical_type(DUCKDB_TYPE_BIGINT));
    duckdb_bind_add_result_column(info, "val", duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_bind_set_bind_data(info, d, free);
}

static void tbt_init(duckdb_init_info info) {
    LUA_LOCK();
    tbt_data_t *d = (tbt_data_t *)duckdb_init_get_bind_data(info);
    tbt_state_t *s = (tbt_state_t *)duckdb_malloc(sizeof(tbt_state_t));
    memset(s, 0, sizeof(*s));
    duckdb_init_set_init_data(info, s, free);

    if (d && d->source) {
        L_(); lua_State *L = g_lua;
        if (!L) { LUA_UNLOCK(); return; }

        /* Try compiled UDF first, then inline source */
        lua_getglobal(L, d->source);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            /* Compile inline source → function on stack */
            if (luaL_loadstring(L, d->source) != LUA_OK) { lua_pop(L, 1); LUA_UNLOCK(); return; }
            if (lua_pcall(L, 0, 1, 0) != LUA_OK) { lua_pop(L, 1); LUA_UNLOCK(); return; }
        }
        /* Now call the function (compiled or inline) */
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); LUA_UNLOCK(); return; }
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) { lua_pop(L, 1); LUA_UNLOCK(); return; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); LUA_UNLOCK(); return; }

        int n = (int)lua_objlen(L, -1);
        s->nrows = n;
        s->rows = (char **)duckdb_malloc(n * sizeof(char *));
        for (int i = 0; i < n; i++) {
            lua_rawgeti(L, -1, i + 1);
            to_str(L);
            s->rows[i] = strdup(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1); /* pop result table */
    }
    LUA_UNLOCK();
}

static void tbt_func(duckdb_function_info fi, duckdb_data_chunk out) {
    tbt_state_t *s = (tbt_state_t *)duckdb_function_get_init_data(fi);
    if (!s || s->cur_row >= s->nrows) { duckdb_data_chunk_set_size(out, 0); return; }
    duckdb_data_chunk_set_size(out, 1);
    ((int64_t *)duckdb_vector_get_data(duckdb_data_chunk_get_vector(out, 0)))[0] = s->cur_row + 1;
    duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(out, 1), 0,
        s->rows ? s->rows[s->cur_row] : "NIL");
    s->cur_row++;
}

/* ── luajit_module() table function ── */

typedef struct {
    char *mode, *source, *sql_name, *msg, *detail, *phase;
    bool ok;
} bind_t;
typedef struct { bool done; } state_t;

static void mod_bind(duckdb_bind_info info) {
    bind_t *d=(bind_t*)duckdb_malloc(sizeof(bind_t));
    memset(d,0,sizeof(*d));d->mode=strdup("info");
    #define G(n,f)do{duckdb_value v=duckdb_bind_get_named_parameter(info,n);\
        if(v){free(d->f);d->f=strdup(duckdb_get_varchar(v));duckdb_destroy_value(&v);}}while(0)
    G("mode",mode);G("source",source);G("sql_name",sql_name);
    #undef G
    duckdb_bind_add_result_column(info,"ok",duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN));
    duckdb_bind_add_result_column(info,"mode",duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_bind_add_result_column(info,"phase",duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_bind_add_result_column(info,"message",duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_bind_add_result_column(info,"detail",duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_bind_add_result_column(info,"sql_name",duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_bind_set_bind_data(info,d,free);
}

/* mod_init_locked: full logic (all modes). Caller holds LUA_LOCK. */
static void mod_init_locked(duckdb_init_info info) {
    bind_t *d=(bind_t*)duckdb_init_get_bind_data(info);
    if(!d)return;
    state_t *s=(state_t*)duckdb_malloc(sizeof(state_t));
    memset(s,0,sizeof(*s));duckdb_init_set_init_data(info,s,free);

    const char *m=d->mode?d->mode:"info";

    /* ── info mode ── */
    if(!strcmp(m,"info")){d->ok=true;d->phase="info";return;}

    /* ── inspect mode: show function details ── */
    if(!strcmp(m,"inspect")){
        if(!d->sql_name){d->msg=strdup("need sql_name");return;}
        L_();lua_State *L=g_lua;
        lua_getglobal(L,d->sql_name);
        if(!lua_isfunction(L,-1)){d->msg=strdup("not found");lua_pop(L,1);return;}
        /* Probe arity */
        int np=0,isvararg=0;const char *src="?";
        lua_getglobal(L,"debug");lua_getfield(L,-1,"getinfo");
        lua_pushvalue(L,-3);lua_pushstring(L,"uS");
        if(lua_pcall(L,2,1,0)==LUA_OK&&lua_istable(L,-1)){
            lua_getfield(L,-1,"nparams");np=(int)lua_tointeger(L,-1);lua_pop(L,1);
            lua_getfield(L,-1,"isvararg");isvararg=lua_toboolean(L,-1);lua_pop(L,1);
            lua_getfield(L,-1,"source");if(lua_isstring(L,-1))src=lua_tostring(L,-1);lua_pop(L,1);
        }
        lua_pop(L,2);
        /* Guess return type by calling with zero/nil args */
        const char *ret="VARCHAR";
        lua_pushvalue(L,-1); /* dup function */
        for(int i=0;i<np;i++)lua_pushinteger(L,0);
        if(lua_pcall(L,np,1,0)==LUA_OK){
            if(lua_isboolean(L,-1))ret="BOOLEAN";
            else if(lua_isnumber(L,-1)){
                lua_Number n=lua_tonumber(L,-1);
                ret=(n==(lua_Number)(lua_Integer)n)?"BIGINT":"DOUBLE";
            }
            lua_pop(L,1);
        }else{lua_pop(L,1);}
        lua_pop(L,1);
        char buf[256];
        snprintf(buf,sizeof(buf),"%s(%d arg%s%s) -> %s (source: %s)",d->sql_name,np,
            np!=1?"s":"",isvararg?", ...":"",ret,src);
        d->ok=true;d->phase="inspect";d->sql_name=strdup(d->sql_name);d->detail=strdup(buf);
        return;
    }

    /* ── list mode: enumerate compiled UDFs ── */
    if(!strcmp(m,"list")){
        L_();lua_State *L=g_lua;
        lua_getglobal(L,"_G");if(!lua_istable(L,-1)){d->msg=strdup("no globals");lua_pop(L,1);return;}
        char buf[4096]="";int off=0;
        lua_pushnil(L);
        while(lua_next(L,-2)){
            if(lua_isfunction(L,-1)&&lua_type(L,-2)==LUA_TSTRING){
                const char *k=lua_tostring(L,-2);
                /* skip Lua builtins — only show user-compiled UDFs */
                if(k[0]!='_' /* skip __pairs etc */
                   && strcmp(k,"assert")&&strcmp(k,"collectgarbage")&&strcmp(k,"dofile")
                   && strcmp(k,"error")&&strcmp(k,"gcinfo")&&strcmp(k,"getfenv")
                   && strcmp(k,"getmetatable")&&strcmp(k,"ipairs")&&strcmp(k,"load")
                   && strcmp(k,"loadfile")&&strcmp(k,"loadstring")&&strcmp(k,"module")
                   && strcmp(k,"newproxy")&&strcmp(k,"next")&&strcmp(k,"pairs")
                   && strcmp(k,"pcall")&&strcmp(k,"print")&&strcmp(k,"rawequal")
                   && strcmp(k,"rawget")&&strcmp(k,"rawset")&&strcmp(k,"require")
                   && strcmp(k,"select")&&strcmp(k,"setfenv")&&strcmp(k,"setmetatable")
                   && strcmp(k,"tonumber")&&strcmp(k,"tostring")&&strcmp(k,"type")
                   && strcmp(k,"unpack")&&strcmp(k,"xpcall"))
                    off+=snprintf(buf+off,sizeof(buf)-off,"%s%s",off?", ":"",k);
            }
            lua_pop(L,1);
        }
        lua_pop(L,1);
        d->ok=true;d->phase="list";d->msg=strdup("UDFs");d->detail=strdup(buf[0]?buf:"(none)");
        d->sql_name=strdup(buf[0]?buf:"");
        return;
    }

    /* ── drop mode: remove a compiled UDF ── */
    if(!strcmp(m,"drop")){
        if(!d->sql_name){d->msg=strdup("need sql_name");return;}
        L_();lua_State *L=g_lua;
        lua_pushnil(L);lua_setglobal(L,d->sql_name);
        d->ok=true;d->phase="drop";d->sql_name=strdup(d->sql_name);
        return;
    }

    /* ── reset mode: clear all UDFs ── */
    if(!strcmp(m,"reset")){
        L_();if(g_lua){lua_close(g_lua);g_lua=NULL;}L_();
        d->ok=true;d->phase="reset";d->msg=strdup("Lua state reset");
        return;
    }

    /* ── save mode: persist UDFs to file ── */
    if(!strcmp(m,"save")){
        const char *path = d->source ? d->source : "luajit_udfs.txt";
        L_();lua_State *L=g_lua;
        if(!L){d->msg=strdup("no Lua");return;}

        FILE *fp=fopen(path,"w");
        if(!fp){d->msg=strdup("file write failed");return;}

        /* Iterate globals, write name\\tsource per line */
        lua_getglobal(L,"_G");lua_pushnil(L);
        int n_saved=0;
        while(lua_next(L,-2)){
            if(lua_isfunction(L,-1)&&lua_type(L,-2)==LUA_TSTRING){
                const char *k=lua_tostring(L,-2);
                if(k[0]!='_'
                   &&strcmp(k,"assert")&&strcmp(k,"collectgarbage")&&strcmp(k,"dofile")
                   &&strcmp(k,"error")&&strcmp(k,"gcinfo")&&strcmp(k,"getfenv")
                   &&strcmp(k,"getmetatable")&&strcmp(k,"ipairs")&&strcmp(k,"load")
                   &&strcmp(k,"loadfile")&&strcmp(k,"loadstring")&&strcmp(k,"module")
                   &&strcmp(k,"newproxy")&&strcmp(k,"next")&&strcmp(k,"pairs")
                   &&strcmp(k,"pcall")&&strcmp(k,"print")&&strcmp(k,"rawequal")
                   &&strcmp(k,"rawget")&&strcmp(k,"rawset")&&strcmp(k,"require")
                   &&strcmp(k,"select")&&strcmp(k,"setfenv")&&strcmp(k,"setmetatable")
                   &&strcmp(k,"tonumber")&&strcmp(k,"tostring")&&strcmp(k,"type")
                   &&strcmp(k,"unpack")&&strcmp(k,"xpcall"))
                {
                    /* Get source from _UDF_SOURCES registry */
                    const char *src="?";
                    lua_getglobal(L,"_UDF_SOURCES");
                    if(lua_istable(L,-1)){lua_getfield(L,-1,k);if(lua_isstring(L,-1))src=lua_tostring(L,-1);lua_pop(L,1);}
                    lua_pop(L,1);
                    fprintf(fp,"%s\t%s\n",k,src);
                    n_saved++;
                }
            }
            lua_pop(L,1);
        }
        lua_pop(L,1);
        fclose(fp);

        d->ok=true;d->phase="save";
        char buf[256];snprintf(buf,sizeof(buf),"saved %d UDFs to %s",n_saved,path);
        d->msg=strdup(buf);
        return;
    }

    /* ── load mode: restore UDFs from file ── */
    if(!strcmp(m,"load")){
        const char *path = d->source ? d->source : "luajit_udfs.txt";
        int loaded = load_udfs_from_file(path);
        d->ok=true;d->phase="load";
        char mbuf[128];snprintf(mbuf,sizeof(mbuf),"loaded %d UDFs from %s",loaded,path);
        d->msg=strdup(mbuf);
        return;
    }

    /* ── macro mode ── */
    if(!strcmp(m,"macro")){
        if(!d->sql_name){d->msg=strdup("need sql_name for macro");return;}
        const char *var="v";
        if(d->source){
            if(!strcmp(d->source,"i")||!strcmp(d->source,"BIGINT"))var="i";
            else if(!strcmp(d->source,"f")||!strcmp(d->source,"DOUBLE"))var="f";
            else if(!strcmp(d->source,"b")||!strcmp(d->source,"BOOLEAN"))var="b";
            else if(!strcmp(d->source,"m")||!strcmp(d->source,"MIXED"))var="m";
        }
        const char *fn=*var=='i'?"luajit_i":*var=='f'?"luajit_f":*var=='b'?"luajit_b":*var=='m'?"luajit_m":"luajit";
        char buf[4096];int off=snprintf(buf,sizeof(buf),"CREATE OR REPLACE MACRO %s(",d->sql_name);
        int nargs=2;
        L_();lua_State *L=g_lua;lua_getglobal(L,d->sql_name);
        if(lua_isfunction(L,-1)){
            lua_getglobal(L,"debug");lua_getfield(L,-1,"getinfo");
            lua_pushvalue(L,-3);lua_pushstring(L,"u");
            if(lua_pcall(L,2,1,0)==LUA_OK&&lua_istable(L,-1))
                {lua_getfield(L,-1,"nparams");nargs=(int)lua_tointeger(L,-1);lua_pop(L,1);}
            lua_pop(L,2);
        }
        lua_pop(L,1);
        for(int i=0;i<nargs;i++)off+=snprintf(buf+off,sizeof(buf)-off,"%sx%d",i>0?", ":"",i+1);
        off+=snprintf(buf+off,sizeof(buf)-off,") AS %s('",fn);
        off+=snprintf(buf+off,sizeof(buf)-off,"%s'",d->sql_name);
        for(int i=0;i<nargs;i++)off+=snprintf(buf+off,sizeof(buf)-off,", x%d",i+1);
        snprintf(buf+off,sizeof(buf)-off,")");
        d->ok=true;d->phase="macro";d->detail=strdup(buf);d->sql_name=strdup(d->sql_name);
        return;
    }

    /* ── quick_compile: compile + auto-probe return type + auto-macro ── */
    if(!strcmp(m,"quick_compile")){
        if(!d->source||!d->sql_name){d->msg=strdup("need source+sql_name for quick_compile");return;}
        L_();lua_State *L=g_lua;
        if(!L){d->msg=strdup("no Lua");return;}
        d->phase="quick_compile";
        /* Step 1: compile source */
        if(luaL_loadstring(L,d->source)!=LUA_OK)
            {d->msg=strdup(lua_tostring(L,-1));lua_pop(L,1);return;}
        if(lua_pcall(L,0,1,0)!=LUA_OK)
            {d->msg=strdup(lua_tostring(L,-1));lua_pop(L,1);return;}
        if(!lua_isfunction(L,-1))
            {d->msg=strdup("must return a function");lua_pop(L,1);return;}
        /* Step 2: probe arity */
        int np=0;
        lua_pushvalue(L,-1);
        lua_getglobal(L,"debug");lua_getfield(L,-1,"getinfo");
        lua_pushvalue(L,-3);lua_pushstring(L,"u");
        if(lua_pcall(L,2,1,0)==LUA_OK&&lua_istable(L,-1))
            {lua_getfield(L,-1,"nparams");np=(int)lua_tointeger(L,-1);lua_pop(L,1);}
        lua_pop(L,2);lua_pop(L,1);
        /* Step 3: probe return type by calling with zero ints */
        const char *var="v"; /* v=f(0,...) → VARCHAR */
        lua_pushvalue(L,-1);
        for(int i=0;i<np;i++)lua_pushinteger(L,0);
        if(lua_pcall(L,np,1,0)==LUA_OK){
            if(lua_isboolean(L,-1))var="b";
            else if(lua_isnumber(L,-1)){
                lua_Number n=lua_tonumber(L,-1);
                var=(n==(lua_Number)(lua_Integer)n)?"i":"f";
            }
            lua_pop(L,1);
        }else{lua_pop(L,1);}
        /* Step 4: store as Lua global */
        /* Step 6: save source for persistence */
        lua_getglobal(L,"_UDF_SOURCES");
        if(!lua_istable(L,-1)){lua_pop(L,1);lua_newtable(L);lua_setglobal(L,"_UDF_SOURCES");lua_getglobal(L,"_UDF_SOURCES");}
        lua_pushstring(L,d->source);lua_setfield(L,-2,d->sql_name);
        lua_pop(L,1);

        lua_setglobal(L,d->sql_name);
        /* Step 5: generate macro DDL */
        const char *fn=*var=='i'?"luajit_i":*var=='f'?"luajit_f":*var=='b'?"luajit_b":"luajit";
        char buf[2048];int off=snprintf(buf,sizeof(buf),"CREATE OR REPLACE MACRO %s(",d->sql_name);
        for(int i=0;i<np;i++)off+=snprintf(buf+off,sizeof(buf)-off,"%sx%d",i>0?", ":"",i+1);
        off+=snprintf(buf+off,sizeof(buf)-off,") AS %s('",fn);
        off+=snprintf(buf+off,sizeof(buf)-off,"%s'",d->sql_name);
        for(int i=0;i<np;i++)off+=snprintf(buf+off,sizeof(buf)-off,", x%d",i+1);
        snprintf(buf+off,sizeof(buf)-off,")");

        d->ok=true;d->sql_name=strdup(d->sql_name);d->detail=strdup(buf);
        return;
    }

    /* ── compile mode ── */
    if(strcmp(m,"compile")){d->msg=strdup("bad mode");return;}
    if(!d->source||!d->sql_name){d->msg=strdup("need source+sql_name");return;}
    L_();lua_State *L=g_lua;
    if(!L){d->msg=strdup("no Lua");return;}
    d->phase="compile";
    if(luaL_loadstring(L,d->source)!=LUA_OK)
        {d->msg=strdup(lua_tostring(L,-1));lua_pop(L,1);return;}
    if(lua_pcall(L,0,1,0)!=LUA_OK)
        {d->msg=strdup(lua_tostring(L,-1));lua_pop(L,1);return;}
    if(!lua_isfunction(L,-1))
        {d->msg=strdup("must return a function");lua_pop(L,1);return;}
    lua_setglobal(L,d->sql_name);
    /* save source for persistence */
    lua_getglobal(L,"_UDF_SOURCES");
    if(!lua_istable(L,-1)){lua_pop(L,1);lua_newtable(L);lua_setglobal(L,"_UDF_SOURCES");lua_getglobal(L,"_UDF_SOURCES");}
    lua_pushstring(L,d->source);lua_setfield(L,-2,d->sql_name);lua_pop(L,1);
    d->ok=true;d->sql_name=strdup(d->sql_name);
}

/* Thread-safe entry: all luajit_module modes touch the shared Lua state. */
static void mod_init(duckdb_init_info info) {
    LUA_LOCK();
    mod_init_locked(info);
    LUA_UNLOCK();
}

static void mod_func(duckdb_function_info fi, duckdb_data_chunk out) {
    bind_t *d=(bind_t*)duckdb_function_get_bind_data(fi);
    state_t *s=(state_t*)duckdb_function_get_init_data(fi);
    if(!d||!s||s->done){duckdb_data_chunk_set_size(out,0);return;}
    duckdb_data_chunk_set_size(out,1);
    #define C(n,val) duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(out,n),0,val?val:"")
    ((bool*)duckdb_vector_get_data(duckdb_data_chunk_get_vector(out,0)))[0]=d->ok;
    C(1,d->mode);C(2,d->phase);C(3,d->msg?d->msg:(d->ok?"OK":"ERROR"));
    C(4,d->detail);C(5,d->sql_name);
    #undef C
    s->done=true;
}

/* ── Registration ── */

void luajit_register_module_functions(
    duckdb_connection conn, duckdb_extension_info ei, struct duckdb_extension_access *acc)
{
    (void)ei;(void)acc;
    lua_lock_init();
    L_();

    /* Create persistent connection for Lua→DuckDB callbacks */
    if (!g_conn) {
        duckdb_database *db = acc->get_database(ei);
        if (db) duckdb_connect(*db, &g_conn);
    }

    #define REG(name,fn,rt) do{\
        duckdb_scalar_function f=duckdb_create_scalar_function();\
        duckdb_scalar_function_set_name(f,name);\
        duckdb_scalar_function_set_function(f,fn);\
        duckdb_scalar_function_set_return_type(f,duckdb_create_logical_type(rt));\
        duckdb_scalar_function_add_parameter(f,duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));

    #define VARGS(t) duckdb_scalar_function_set_varargs(f,duckdb_create_logical_type(t));
    #define END() duckdb_register_scalar_function(conn,f);duckdb_destroy_scalar_function(&f);}while(0)

    REG("luajit",   fj,  DUCKDB_TYPE_VARCHAR) END();
    REG("luajit_i", fji, DUCKDB_TYPE_BIGINT)  VARGS(DUCKDB_TYPE_BIGINT)  END();
    REG("luajit_f", fjf, DUCKDB_TYPE_DOUBLE)  VARGS(DUCKDB_TYPE_DOUBLE)  END();
    REG("luajit_b", fjb, DUCKDB_TYPE_BOOLEAN) VARGS(DUCKDB_TYPE_BOOLEAN) END();
    REG("luajit_m", fjm, DUCKDB_TYPE_DOUBLE)  VARGS(DUCKDB_TYPE_DOUBLE)  END();
    REG("luajit_v", fjv, DUCKDB_TYPE_DOUBLE)  VARGS(DUCKDB_TYPE_DOUBLE)  END();

    /* luajit_l: LIST(BIGINT) args → LIST(DOUBLE) return */
    { duckdb_scalar_function f = duckdb_create_scalar_function();
      duckdb_scalar_function_set_name(f, "luajit_l");
      duckdb_scalar_function_set_function(f, fjl);
      duckdb_logical_type rt = duckdb_create_list_type(duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE));
      duckdb_scalar_function_set_return_type(f, rt);
      duckdb_destroy_logical_type(&rt);
      duckdb_scalar_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_logical_type at = duckdb_create_list_type(duckdb_create_logical_type(DUCKDB_TYPE_BIGINT));
      duckdb_scalar_function_set_varargs(f, at);
      duckdb_destroy_logical_type(&at);
      duckdb_register_scalar_function(conn, f);
      duckdb_destroy_scalar_function(&f); }

    /* luajit_s: STRUCT → VARCHAR bridge (ANY param for generic struct) */
    { duckdb_scalar_function f = duckdb_create_scalar_function();
      duckdb_scalar_function_set_name(f, "luajit_s");
      duckdb_scalar_function_set_function(f, fjs);
      duckdb_scalar_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_scalar_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_ANY));
      duckdb_scalar_function_set_return_type(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_register_scalar_function(conn, f);
      duckdb_destroy_scalar_function(&f); }

    /* luajit_map: MAP(ANY,ANY) → VARCHAR bridge */
    { duckdb_scalar_function f = duckdb_create_scalar_function();
      duckdb_scalar_function_set_name(f, "luajit_map");
      duckdb_scalar_function_set_function(f, fjm_map);
      duckdb_scalar_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_scalar_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_ANY));
      duckdb_scalar_function_set_return_type(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_register_scalar_function(conn, f);
      duckdb_destroy_scalar_function(&f); }

    /* luajit_agg: aggregate UDF — accumulate values, call Lua on finalize */
    { duckdb_aggregate_function f = duckdb_create_aggregate_function();
      duckdb_aggregate_function_set_name(f, "luajit_agg");
      duckdb_aggregate_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_aggregate_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE));
      duckdb_aggregate_function_set_return_type(f, duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE));
      duckdb_aggregate_function_set_functions(f, agg_state_size, agg_init, agg_update, agg_combine, agg_finalize);
      { int *es = (int *)duckdb_malloc(sizeof(int));
        *es = 42;
        duckdb_aggregate_function_set_extra_info(f, es, free); }
      duckdb_register_aggregate_function(conn, f);
      duckdb_destroy_aggregate_function(&f); }
    #undef REG
    #undef VARGS
    #undef END

    /* ── luajit_table ── */
    { duckdb_table_function t = duckdb_create_table_function();
      duckdb_table_function_set_name(t, "luajit_table");
      duckdb_table_function_add_parameter(t, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
      duckdb_table_function_set_bind(t, tbt_bind);
      duckdb_table_function_set_init(t, tbt_init);
      duckdb_table_function_set_function(t, tbt_func);
      duckdb_register_table_function(conn, t);
      duckdb_destroy_table_function(&t); }

    /* ── luajit_module ── */

    duckdb_table_function t=duckdb_create_table_function();
    duckdb_table_function_set_name(t,"luajit_module");
    duckdb_table_function_add_named_parameter(t,"mode",duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_table_function_add_named_parameter(t,"source",duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_table_function_add_named_parameter(t,"sql_name",duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_table_function_set_bind(t,mod_bind);
    duckdb_table_function_set_init(t,mod_init);
    duckdb_table_function_set_function(t,mod_func);
    duckdb_register_table_function(conn,t);
    duckdb_destroy_table_function(&t);

    /* Auto-load UDFs from default file on extension init */
    load_udfs_from_file("luajit_udfs.txt");
}

#endif /* !LUAJIT_WASM_STUB */
