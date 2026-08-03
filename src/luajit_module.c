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

/* MSVC lacks __attribute__((noinline)); use __declspec(noinline) there. */
#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

/* ── Thread safety: single global Lua state, guarded by a recursive mutex ──
 * DuckDB executes UDFs in parallel across threads; every Lua access must be
 * serialized. Recursive so _duckdb_call → duckdb_query → luajit UDF can re-enter.
 */
#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_udf_lock;
static void lua_lock_init(void) { InitializeCriticalSection(&g_udf_lock); }
#define LUA_LOCK()   EnterCriticalSection(&g_udf_lock)
#define LUA_UNLOCK() LeaveCriticalSection(&g_udf_lock)
#else
#include <pthread.h>
static pthread_mutex_t g_udf_lock;
static void lua_lock_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_udf_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}
#define LUA_LOCK()   pthread_mutex_lock(&g_udf_lock)
#define LUA_UNLOCK() pthread_mutex_unlock(&g_udf_lock)
#endif

/* ── P4: per-thread lua_State + shared UDF source table ──
 * Each worker thread gets its own lua_State via TLS — Lua execution is no
 * longer serialized by a global lock, so DuckDB thread parallelism maps
 * directly to LuaJIT parallelism (one JIT per thread).
 *
 * UDF *sources* live in a shared C table (g_udf_sources, guarded by
 * g_udf_lock). Each state compiles lazily on first use (resolve_udf_chunk
 * falls back to the shared table), so compile/drop/reset need only touch the
 * shared table + the calling thread's state.
 *
 * LUA_LOCK now guards SHARED data only (udf table, g_last_error, g_trusted,
 * g_conn) — never Lua state execution. It stays recursive so existing
 * nesting (mod_init_locked → load_udfs_from_file) cannot deadlock.
 */
#if defined(_MSC_VER)
#define LUA_TLS __declspec(thread)
#else
#define LUA_TLS __thread
#endif

static LUA_TLS lua_State *g_lua = NULL;
static duckdb_connection g_conn = NULL;
/* Last Lua UDF runtime error — set/read under LUA_LOCK. We do NOT use
 * duckdb_function_set_error: on DuckDB 1.2.0 the CFunctionInfo error string
 * handed to us is in a corrupted state (crash inside std::string::assign).
 * This is our own stable error channel instead. */
static char *g_last_error = NULL;
/* Trusted/sandbox mode (N1 from pllua study): when on, dangerous globals
 * (io, ffi, package, require, load*, debug) are removed and os is reduced to
 * date/clock/time/difftime. Originals are stashed in the registry so the
 * mode can be toggled back off. Toggle via luajit_module(mode:='trusted',
 * source:='on'|'off'). Guarded by LUA_LOCK; applied to every state (existing
 * + newly created in L_()). */
static bool g_trusted = false;

/* Shared UDF source table: name → source. Compiled per-state on demand. */
#define MAX_UDFS 256
typedef struct { char *name; char *source; } udf_src_t;
static udf_src_t g_udf_sources[MAX_UDFS];
static int g_udf_count = 0;

/* Callers must hold LUA_LOCK. Returns strdup'd source or NULL. */
static char *udf_source_get(const char *name) {
    for (int i = 0; i < g_udf_count; i++)
        if (!strcmp(g_udf_sources[i].name, name))
            return strdup(g_udf_sources[i].source);
    return NULL;
}
/* Callers must hold LUA_LOCK. Insert or replace. */
static void udf_source_set(const char *name, const char *source) {
    for (int i = 0; i < g_udf_count; i++) {
        if (!strcmp(g_udf_sources[i].name, name)) {
            free(g_udf_sources[i].source);
            g_udf_sources[i].source = strdup(source);
            return;
        }
    }
    if (g_udf_count < MAX_UDFS) {
        g_udf_sources[g_udf_count].name = strdup(name);
        g_udf_sources[g_udf_count].source = strdup(source);
        g_udf_count++;
    }
}
/* Callers must hold LUA_LOCK. Returns true if removed. */
static bool udf_source_del(const char *name) {
    for (int i = 0; i < g_udf_count; i++) {
        if (!strcmp(g_udf_sources[i].name, name)) {
            free(g_udf_sources[i].name);
            free(g_udf_sources[i].source);
            g_udf_sources[i] = g_udf_sources[g_udf_count - 1];
            g_udf_count--;
            return true;
        }
    }
    return false;
}

/* ── Executor entry guard ──
 * P4: no global lock (each thread owns its lua_State via TLS). L_() lazily
 * creates the state; goto lua_cleanup on failure. Shared data (udf table,
 * last_error) is locked at the point of use, not here.
 * Trusted mode is applied lazily per state: each executor checks the state's
 * lj_trusted_applied registry flag against the global g_trusted and applies
 * the sandbox on first touch (threads that created their state before the
 * toggle still get sandboxed).
 */
#define LUA_BEGIN()  L_(); lua_State *L = g_lua; if (!L) goto lua_cleanup; \
    lua_getfield(L, LUA_REGISTRYINDEX, "lj_trusted_applied"); \
    int _lj_tr = lua_toboolean(L, -1); lua_pop(L, 1); \
    if (g_trusted && !_lj_tr) apply_trusted(L, true); \
    else if (!g_trusted && _lj_tr) apply_trusted(L, false);
#define LUA_CLEANUP() lua_cleanup:

static void apply_trusted(lua_State *L, bool on) {
    if (!L) return;
    if (on) {
        /* Idempotent: re-applying to an already-sandboxed state is a no-op
         * (dangerous globals are already nil; os already reduced). */
        static const char *dead[] = {"io","ffi","package","require","dofile",
                                     "loadfile","load","loadstring","debug",NULL};
        for (int i = 0; dead[i]; i++) {
            /* stash original */
            lua_getglobal(L, dead[i]);
            if (!lua_isnil(L, -1)) {
                lua_pushvalue(L, -1);
                lua_setfield(L, LUA_REGISTRYINDEX, "lj_orig");
                lua_setfield(L, LUA_REGISTRYINDEX, dead[i]);
            }
            lua_pop(L, 1);
            lua_pushnil(L);
            lua_setglobal(L, dead[i]);
        }
        /* os: keep only date/clock/time/difftime */
        lua_getglobal(L, "os");
        if (lua_istable(L, -1)) {
            static const char *keep[] = {"date","clock","time","difftime"};
            lua_pushvalue(L, -1);
            lua_setfield(L, LUA_REGISTRYINDEX, "lj_orig_os");
            lua_createtable(L, 0, 4);
            for (int i = 0; i < 4; i++) {
                lua_getfield(L, -2, keep[i]);
                lua_setfield(L, -2, keep[i]);
            }
            lua_setglobal(L, "os");
        }
        lua_pop(L, 1);
        g_trusted = true;
        lua_pushboolean(L, 1);
        lua_setfield(L, LUA_REGISTRYINDEX, "lj_trusted_applied");
    } else if (!on) {
        /* Idempotent restore: no-op for states that were never sandboxed
         * (lj_orig_* registry entries absent → globals set to nil, matching
         * their original state). */
        static const char *dead[] = {"io","ffi","package","require","dofile",
                                     "loadfile","load","loadstring","debug",NULL};
        for (int i = 0; dead[i]; i++) {
            lua_getfield(L, LUA_REGISTRYINDEX, "lj_orig");
            lua_getfield(L, LUA_REGISTRYINDEX, dead[i]);
            if (!lua_isnil(L, -1)) lua_setglobal(L, dead[i]);
            else { lua_pop(L, 1); lua_pushnil(L); lua_setglobal(L, dead[i]); }
            lua_pushnil(L);
            lua_setfield(L, LUA_REGISTRYINDEX, dead[i]);
            lua_pop(L, 1);
        }
        lua_getfield(L, LUA_REGISTRYINDEX, "lj_orig_os");
        if (!lua_isnil(L, -1)) lua_setglobal(L, "os");
        else { lua_pop(L, 1); lua_pushnil(L); lua_setglobal(L, "os"); }
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "lj_orig_os");
        lua_pop(L, 1);
        g_trusted = false;
        lua_pushboolean(L, 0);
        lua_setfield(L, LUA_REGISTRYINDEX, "lj_trusted_applied");
    }
}

/* ── DuckDB callback from Lua: _duckdb_call(sql) → status VARCHAR ── */

/* forward decls (defined with the type helpers below; used by _duckdb_query) */
static void push_timestamp_micros(lua_State *L, int64_t micros);
static void push_decimal_any(lua_State *L, duckdb_logical_type lt, void *data, idx_t r);

static int l_duckdb_call(lua_State *L) {
    if (!g_conn) { lua_pushstring(L, "no connection"); return 1; }
    const char *sql = luaL_checkstring(L, 1);
    duckdb_result res;
    LUA_LOCK();  /* P4: g_conn is shared across threads */
    duckdb_state st = duckdb_query(g_conn, sql, &res);
    LUA_UNLOCK();
    if (st != DuckDBSuccess) {
        lua_pushfstring(L, "error: %s", duckdb_result_error(&res));
        duckdb_destroy_result(&res); return 1;
    }
    lua_pushstring(L, "ok");
    duckdb_destroy_result(&res);
    return 1;
}

/* _duckdb_query(sql) → result_table | nil, errmsg
 * Full result-set bridge (SPI-style, from pllua study N2): runs the query and
 * returns { {col=val, ...}, ... } — one Lua table per row, keys = column
 * names. Types: integer family / DOUBLE / FLOAT / BOOLEAN / VARCHAR; NULL → nil.
 * On error returns nil + error message (catchable via pcall). */
static int l_duckdb_query(lua_State *L) {
    const char *sql = luaL_checkstring(L, 1);
    if (!g_conn) { lua_pushnil(L); lua_pushstring(L, "no connection"); return 2; }
    duckdb_result res;
    LUA_LOCK();  /* P4: g_conn is shared across threads */
    duckdb_state st = duckdb_query(g_conn, sql, &res);
    LUA_UNLOCK();
    if (st != DuckDBSuccess) {
        const char *err = duckdb_result_error(&res);
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "query error");
        duckdb_destroy_result(&res);
        return 2;
    }
    /* Extension API v1.2: no result-data readers in stable surface; use the
     * streaming fetch_chunk loop (duckdb_fetch_chunk is stable, unlike the
     * UNSTABLE-gated row_count/column_data/get_chunk). */
    idx_t ncols = duckdb_column_count(&res);
    idx_t out_row = 0;
    lua_createtable(L, 0, 0);
    duckdb_data_chunk chunk;
    while ((chunk = duckdb_fetch_chunk(res)) != NULL) {
        idx_t nrows = duckdb_data_chunk_get_size(chunk);
        for (idx_t r = 0; r < nrows; r++) {
            lua_createtable(L, 0, (int)ncols);
            for (idx_t c = 0; c < ncols; c++) {
                duckdb_vector v = duckdb_data_chunk_get_vector(chunk, c);
                duckdb_type ct = duckdb_get_type_id(duckdb_vector_get_column_type(v));
                void *data = duckdb_vector_get_data(v);
                uint64_t *validity = duckdb_vector_get_validity(v);
                lua_pushstring(L, duckdb_column_name(&res, c));
                if (validity && !duckdb_validity_row_is_valid(validity, r)) {
                    lua_pushnil(L);
                } else if (ct == DUCKDB_TYPE_BIGINT || ct == DUCKDB_TYPE_INTEGER ||
                           ct == DUCKDB_TYPE_SMALLINT || ct == DUCKDB_TYPE_TINYINT ||
                           ct == DUCKDB_TYPE_UBIGINT || ct == DUCKDB_TYPE_UINTEGER ||
                           ct == DUCKDB_TYPE_USMALLINT || ct == DUCKDB_TYPE_UTINYINT) {
                    lua_pushnumber(L, (lua_Number)((int64_t *)data)[r]);
                } else if (ct == DUCKDB_TYPE_DOUBLE || ct == DUCKDB_TYPE_FLOAT) {
                    lua_pushnumber(L, ((double *)data)[r]);
                } else if (ct == DUCKDB_TYPE_BOOLEAN) {
                    lua_pushboolean(L, ((bool *)data)[r]);
                } else if (ct == DUCKDB_TYPE_DATE) {
                    duckdb_date_struct ds = duckdb_from_date(((duckdb_date *)data)[r]);
                    char buf[16]; snprintf(buf, sizeof(buf), "%04d-%02d-%02d", ds.year, ds.month, ds.day);
                    lua_pushstring(L, buf);
                } else if (ct == DUCKDB_TYPE_TIMESTAMP || ct == DUCKDB_TYPE_TIMESTAMP_S ||
                           ct == DUCKDB_TYPE_TIMESTAMP_MS || ct == DUCKDB_TYPE_TIMESTAMP_NS) {
                    int64_t us;
                    if (ct == DUCKDB_TYPE_TIMESTAMP_S)      us = ((duckdb_timestamp_s *)data)[r].seconds * 1000000;
                    else if (ct == DUCKDB_TYPE_TIMESTAMP_MS) us = ((int64_t *)data)[r] * 1000;
                    else if (ct == DUCKDB_TYPE_TIMESTAMP_NS) us = ((int64_t *)data)[r] / 1000;
                    else                                      us = ((duckdb_timestamp *)data)[r].micros;
                    push_timestamp_micros(L, us);
                } else if (ct == DUCKDB_TYPE_DECIMAL) {
                    push_decimal_any(L, duckdb_vector_get_column_type(v), data, r);
                } else if (ct == DUCKDB_TYPE_HUGEINT || ct == DUCKDB_TYPE_UHUGEINT) {
                    lua_pushnumber(L, duckdb_hugeint_to_double(((duckdb_hugeint *)data)[r]));
                } else if (ct == DUCKDB_TYPE_VARCHAR) {
                    duckdb_string_t s = ((duckdb_string_t *)data)[r];
                    lua_pushlstring(L, duckdb_string_t_data(&s), duckdb_string_t_length(s));
                } else {
                    lua_pushstring(L, "(unsupported type)");
                }
                lua_rawset(L, -3);  /* t[colname] = value */
            }
            lua_rawseti(L, -2, (int)(++out_row));
        }
    }
    duckdb_destroy_result(&res);
    return 1;
}

static void L_(void) {
    if (!g_lua) {
        g_lua = luaL_newstate();
        if (g_lua) {
            luaL_openlibs(g_lua);
            /* register DuckDB callbacks */
            lua_pushcfunction(g_lua, l_duckdb_call);
            lua_setglobal(g_lua, "_duckdb_call");
            lua_pushcfunction(g_lua, l_duckdb_query);
            lua_setglobal(g_lua, "_duckdb_query");
            /* P4: apply trusted sandbox to every (new) state if globally on */
            LUA_LOCK();
            bool trusted = g_trusted;
            LUA_UNLOCK();
            if (trusted) apply_trusted(g_lua, true);
        }
    }
}

/* ── helpers ── */

/* forward decls (defined with the executor helpers below; used earlier) */
static void mark_invalid(duckdb_vector out, idx_t r);

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
/* DATE → 'YYYY-MM-DD' (ISO), NULL → nil */
static void push_date(lua_State *L, duckdb_vector v, idx_t r) {
    if(!duckdb_validity_row_is_valid(duckdb_vector_get_validity(v),r)){lua_pushnil(L);return;}
    duckdb_date_struct ds=duckdb_from_date(((duckdb_date*)duckdb_vector_get_data(v))[r]);
    char buf[16];snprintf(buf,sizeof(buf),"%04d-%02d-%02d",ds.year,ds.month,ds.day);
    lua_pushstring(L,buf);
}
/* TIMESTAMP (micros) → 'YYYY-MM-DD HH:MM:SS' */
static void push_timestamp_micros(lua_State *L, int64_t micros) {
    duckdb_timestamp t; t.micros = micros;
    duckdb_timestamp_struct ts = duckdb_from_timestamp(t);
    char buf[32];snprintf(buf,sizeof(buf),"%04d-%02d-%02d %02d:%02d:%02d",
        ts.date.year,ts.date.month,ts.date.day,ts.time.hour,ts.time.min,ts.time.sec);
    lua_pushstring(L,buf);
}
/* TIMESTAMP family (auto unit conversion; NULL → nil) */
static void push_timestamp(lua_State *L, duckdb_vector v, idx_t r) {
    if(!duckdb_validity_row_is_valid(duckdb_vector_get_validity(v),r)){lua_pushnil(L);return;}
    duckdb_type dt=duckdb_get_type_id(duckdb_vector_get_column_type(v));
    int64_t us;
    if(dt==DUCKDB_TYPE_TIMESTAMP_S)      us=((duckdb_timestamp_s*)duckdb_vector_get_data(v))[r].seconds*1000000;
    else if(dt==DUCKDB_TYPE_TIMESTAMP_MS)us=((int64_t*)duckdb_vector_get_data(v))[r]*1000;
    else if(dt==DUCKDB_TYPE_TIMESTAMP_NS)us=((int64_t*)duckdb_vector_get_data(v))[r]/1000;
    else                                 us=((duckdb_timestamp*)duckdb_vector_get_data(v))[r].micros;
    push_timestamp_micros(L,us);
}
/* DECIMAL → Lua number. DuckDB stores DECIMAL physically by width:
 * width<=4 → int16, 5-9 → int32, 10-18 → int64, 19-38 → hugeint(128).
 * Read the raw storage at the type's true element width, then divide by 10^scale. */
static void push_decimal_any(lua_State *L, duckdb_logical_type lt, void *data, idx_t r) {
    uint8_t scale = duckdb_decimal_scale(lt);
    uint8_t width = duckdb_decimal_width(lt);
    double val;
    if (width <= 4)       val = (double)((int16_t *)data)[r];
    else if (width <= 9)  val = (double)((int32_t *)data)[r];
    else if (width <= 18) val = (double)((int64_t *)data)[r];
    else                  val = duckdb_hugeint_to_double(((duckdb_hugeint *)data)[r]);
    double p = 1.0; for (int i = 0; i < scale; i++) p *= 10.0;
    lua_pushnumber(L, val / p);
}
static void push_decimal(lua_State *L, duckdb_vector v, idx_t r) {
    if(!duckdb_validity_row_is_valid(duckdb_vector_get_validity(v),r)){lua_pushnil(L);return;}
    push_decimal_any(L, duckdb_vector_get_column_type(v), duckdb_vector_get_data(v), r);
}
/* HUGEINT → Lua number (double precision, NULL → nil) */
static void push_hugeint(lua_State *L, duckdb_vector v, idx_t r) {
    if(!duckdb_validity_row_is_valid(duckdb_vector_get_validity(v),r)){lua_pushnil(L);return;}
    lua_pushnumber(L,duckdb_hugeint_to_double(((duckdb_hugeint*)duckdb_vector_get_data(v))[r]));
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
            {mark_invalid(out,r);continue;}
        int ok=luaL_loadbuffer(L,duckdb_string_t_data(&ss),duckdb_string_t_length(ss),"lj");
        ok=ok||lua_pcall(L,0,1,0);
        if(ok){duckdb_vector_assign_string_element(out,r,lua_tostring(L,-1));lua_pop(L,1);continue;}
        if(lua_isnil(L,-1))mark_invalid(out,r);
        else{to_str(L);duckdb_vector_assign_string_element(out,r,lua_tostring(L,-1));lua_pop(L,1);}
        lua_pop(L,1);
    }
    LUA_CLEANUP();
}

/* ── UDF executors ── */

/* ── Chunk-level UDF resolution + error reporting (P2/P1: 2026-08) ──
 * resolve_udf_chunk: resolve UDF name from chunk row 0 → registry ref.
 *   LUA_NOREF if name is NULL or not a function. One global lookup per chunk
 *   instead of per row (old resolve_udf did a lua_getglobal for every row).
 */
static int resolve_udf_chunk(lua_State *L, duckdb_vector nv) {
    duckdb_string_t ns=((duckdb_string_t*)duckdb_vector_get_data(nv))[0];
    if(!duckdb_validity_row_is_valid(duckdb_vector_get_validity(nv),0)) return LUA_NOREF;
    const char *name = duckdb_string_t_data(&ns);
    lua_getglobal(L,name);
    if(lua_isfunction(L,-1)) return luaL_ref(L,LUA_REGISTRYINDEX);
    lua_pop(L,1);
    /* P4: UDF may have been compiled on another thread's state — lazily
     * compile from the shared source table into this thread's state. */
    LUA_LOCK();
    char *src = udf_source_get(name);
    LUA_UNLOCK();
    if (!src) return LUA_NOREF;
    int ref = LUA_NOREF;
    if (luaL_loadstring(L, src) == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK &&
        lua_isfunction(L, -1)) {
        lua_pushvalue(L, -1);
        lua_setglobal(L, name);  /* cache in this state */
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, lua_gettop(L));  /* drop load/compile error */
    }
    free(src);
    return ref;
}

/* Mark all rows of a result vector invalid (unresolved UDF). */
static void invalidate_all(duckdb_vector out, idx_t n) {
    duckdb_vector_ensure_validity_writable(out);
    uint64_t *v=duckdb_vector_get_validity(out);
    memset(v,0,((n+63)/64)*sizeof(uint64_t));
}

/* Call the UDF (already on stack) with nargs args. On Lua error: record the
 * error message into g_last_error (queryable via luajit_module mode
 * 'last_error'); the affected rows are invalidated by the caller.
 * noinline: keep the error path out of the hot loop. */
static NOINLINE int call_udf(duckdb_function_info fi, lua_State *L, int nargs, int nresults) {
    (void)fi;
    if(lua_pcall(L,nargs,nresults,0)!=LUA_OK){
        const char *err=lua_tostring(L,-1);
        char buf[512];
        snprintf(buf,sizeof(buf),"luajit UDF error: %s",err?err:"unknown");
        LUA_LOCK();
        free(g_last_error);
        g_last_error = strdup(buf);
        LUA_UNLOCK();
        lua_pop(L,1);
        return 0;
    }
    return 1;
}

/* Mark one row invalid — MUST ensure the validity mask is writable first:
 * DuckDB's fast path hands out NULL validity for NULL-free vectors, and
 * duckdb_validity_set_row_invalid(NULL) would crash. */
static void mark_invalid(duckdb_vector out, idx_t r) {
    duckdb_vector_ensure_validity_writable(out);
    uint64_t *v=duckdb_vector_get_validity(out);
    duckdb_validity_set_row_invalid(v, r);
}

/* Invalidate rows [r, nr) — used when an UDF error aborts mid-chunk. */
static void invalidate_from(duckdb_vector out, idx_t r, idx_t nr) {
    duckdb_vector_ensure_validity_writable(out);
    uint64_t *v=duckdb_vector_get_validity(out);
    for(idx_t i=r;i<nr;i++) duckdb_validity_set_row_invalid(v,i);
}

/* luajit_i(name, ...BIGINT) → BIGINT */
static void fji(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr=duckdb_data_chunk_get_size(in),nc=duckdb_data_chunk_get_column_count(in);
    int64_t*od=(int64_t*)duckdb_vector_get_data(out);
    int udf_ref=LUA_NOREF;
    if(nr>0)udf_ref=resolve_udf_chunk(L,duckdb_data_chunk_get_vector(in,0));
    if(udf_ref==LUA_NOREF){invalidate_all(out,nr);goto lua_cleanup;}
    for(idx_t r=0;r<nr;r++){
        lua_rawgeti(L,LUA_REGISTRYINDEX,udf_ref);
        for(idx_t c=1;c<nc;c++)push_int(L,duckdb_data_chunk_get_vector(in,c),r);
        if(!call_udf(fi,L,(int)(nc-1),1)){invalidate_from(out,r,nr);break;}
        if(lua_isnil(L,-1)||!lua_isnumber(L,-1))
            mark_invalid(out,r);
        else od[r]=(int64_t)lua_tointeger(L,-1);
        lua_pop(L,1);
    }
    luaL_unref(L,LUA_REGISTRYINDEX,udf_ref);
    LUA_CLEANUP();
}

/* luajit_f(name, ...DOUBLE) → DOUBLE */
static void fjf(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr=duckdb_data_chunk_get_size(in),nc=duckdb_data_chunk_get_column_count(in);
    double*od=(double*)duckdb_vector_get_data(out);
    int udf_ref=LUA_NOREF;
    if(nr>0)udf_ref=resolve_udf_chunk(L,duckdb_data_chunk_get_vector(in,0));
    if(udf_ref==LUA_NOREF){invalidate_all(out,nr);goto lua_cleanup;}
    for(idx_t r=0;r<nr;r++){
        lua_rawgeti(L,LUA_REGISTRYINDEX,udf_ref);
        for(idx_t c=1;c<nc;c++)push_flt(L,duckdb_data_chunk_get_vector(in,c),r);
        if(!call_udf(fi,L,(int)(nc-1),1)){invalidate_from(out,r,nr);break;}
        if(lua_isnil(L,-1)||!lua_isnumber(L,-1))
            mark_invalid(out,r);
        else od[r]=lua_tonumber(L,-1);
        lua_pop(L,1);
    }
    luaL_unref(L,LUA_REGISTRYINDEX,udf_ref);
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
    int udf_ref = resolve_udf_chunk(L, duckdb_data_chunk_get_vector(in, 0));
    if (udf_ref == LUA_NOREF) { invalidate_all(out, nr); goto lua_cleanup; }

    /* Build one Lua table per arg column: {val_1, val_2, ..., val_nr} */
    int nargs = (int)(nc - 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, udf_ref);
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
    if (!call_udf(fi, L, nargs, 1)) { luaL_unref(L, LUA_REGISTRYINDEX, udf_ref); goto lua_cleanup; }
    if (!lua_istable(L, -1)) { lua_pop(L, lua_gettop(L)); luaL_unref(L, LUA_REGISTRYINDEX, udf_ref); goto lua_cleanup; }

    /* Unpack result table → output vector */
    idx_t rlen = (idx_t)lua_objlen(L, -1);
    for (idx_t r = 0; r < nr && r < rlen; r++) {
        lua_rawgeti(L, -1, (int)(r + 1));
        od[r] = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    luaL_unref(L, LUA_REGISTRYINDEX, udf_ref);
    LUA_CLEANUP();
}

/* luajit_vi(name, ...BIGINT) → BIGINT  — chunk-batched: 1 Lua call per chunk.
 * Same batch pattern as luajit_v but for int64 columns: UDF receives
 * {val_1..val_nr} tables, returns a table of results. */
static void fjvi(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr = duckdb_data_chunk_get_size(in);
    idx_t nc = duckdb_data_chunk_get_column_count(in);
    if (nr == 0 || nc < 2) goto lua_cleanup;

    int64_t *od = (int64_t *)duckdb_vector_get_data(out);

    int udf_ref = resolve_udf_chunk(L, duckdb_data_chunk_get_vector(in, 0));
    if (udf_ref == LUA_NOREF) { invalidate_all(out, nr); goto lua_cleanup; }

    int nargs = (int)(nc - 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, udf_ref);
    for (int c = 1; c < (int)nc; c++) {
        lua_createtable(L, (int)nr, 0);
        duckdb_vector v = duckdb_data_chunk_get_vector(in, (idx_t)c);
        int64_t *d = (int64_t *)duckdb_vector_get_data(v);
        for (idx_t r = 0; r < nr; r++) {
            lua_pushinteger(L, (lua_Integer)d[r]);
            lua_rawseti(L, -2, (int)(r + 1));
        }
    }

    if (!call_udf(fi, L, nargs, 1)) { luaL_unref(L, LUA_REGISTRYINDEX, udf_ref); goto lua_cleanup; }
    if (!lua_istable(L, -1)) { lua_pop(L, lua_gettop(L)); luaL_unref(L, LUA_REGISTRYINDEX, udf_ref); goto lua_cleanup; }

    idx_t rlen = (idx_t)lua_objlen(L, -1);
    for (idx_t r = 0; r < nr && r < rlen; r++) {
        lua_rawgeti(L, -1, (int)(r + 1));
        if (lua_isnil(L, -1)) mark_invalid(out, r);
        else od[r] = lua_isnumber(L, -1) ? (int64_t)lua_tointeger(L, -1) : 0;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    luaL_unref(L, LUA_REGISTRYINDEX, udf_ref);
    LUA_CLEANUP();
}

/* luajit_vs(name, ...VARCHAR) → VARCHAR  — chunk-batched for strings.
 * UDF receives {s1..snr} tables, returns a table of strings/nils. */
static void fjvs(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr = duckdb_data_chunk_get_size(in);
    idx_t nc = duckdb_data_chunk_get_column_count(in);
    if (nr == 0 || nc < 2) goto lua_cleanup;

    int udf_ref = resolve_udf_chunk(L, duckdb_data_chunk_get_vector(in, 0));
    if (udf_ref == LUA_NOREF) { invalidate_all(out, nr); goto lua_cleanup; }

    int nargs = (int)(nc - 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, udf_ref);
    for (int c = 1; c < (int)nc; c++) {
        lua_createtable(L, (int)nr, 0);
        duckdb_vector v = duckdb_data_chunk_get_vector(in, (idx_t)c);
        duckdb_string_t *d = (duckdb_string_t *)duckdb_vector_get_data(v);
        for (idx_t r = 0; r < nr; r++) {
            if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(v), r)) {
                lua_pushnil(L);
            } else {
                duckdb_string_t s = d[r];
                lua_pushlstring(L, duckdb_string_t_data(&s), duckdb_string_t_length(s));
            }
            lua_rawseti(L, -2, (int)(r + 1));
        }
    }

    if (!call_udf(fi, L, nargs, 1)) { luaL_unref(L, LUA_REGISTRYINDEX, udf_ref); goto lua_cleanup; }
    if (!lua_istable(L, -1)) { lua_pop(L, lua_gettop(L)); luaL_unref(L, LUA_REGISTRYINDEX, udf_ref); goto lua_cleanup; }

    idx_t rlen = (idx_t)lua_objlen(L, -1);
    for (idx_t r = 0; r < nr && r < rlen; r++) {
        lua_rawgeti(L, -1, (int)(r + 1));
        if (lua_isnil(L, -1)) mark_invalid(out, r);
        else duckdb_vector_assign_string_element(out, r, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    luaL_unref(L, LUA_REGISTRYINDEX, udf_ref);
    LUA_CLEANUP();
}

/* luajit_b(name, ...BOOLEAN) → BOOLEAN */
static void fjb(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr=duckdb_data_chunk_get_size(in),nc=duckdb_data_chunk_get_column_count(in);
    bool*od=(bool*)duckdb_vector_get_data(out);
    int udf_ref=LUA_NOREF;
    if(nr>0)udf_ref=resolve_udf_chunk(L,duckdb_data_chunk_get_vector(in,0));
    if(udf_ref==LUA_NOREF){invalidate_all(out,nr);goto lua_cleanup;}
    for(idx_t r=0;r<nr;r++){
        lua_rawgeti(L,LUA_REGISTRYINDEX,udf_ref);
        for(idx_t c=1;c<nc;c++)push_bool(L,duckdb_data_chunk_get_vector(in,c),r);
        if(!call_udf(fi,L,(int)(nc-1),1)){invalidate_from(out,r,nr);break;}
        if(lua_isnil(L,-1))mark_invalid(out,r);
        else if(lua_isboolean(L,-1))od[r]=lua_toboolean(L,-1)?true:false;
        else if(lua_isnumber(L,-1))od[r]=lua_tonumber(L,-1)!=0.0;
        else od[r]=lua_toboolean(L,-1)?true:false;
        lua_pop(L,1);
    }
    luaL_unref(L,LUA_REGISTRYINDEX,udf_ref);
    LUA_CLEANUP();
}

/* luajit_m(name, ...DOUBLE) → DOUBLE  (mixed-type: all args auto-cast to double) */
static void fjm(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr=duckdb_data_chunk_get_size(in),nc=duckdb_data_chunk_get_column_count(in);
    double*od=(double*)duckdb_vector_get_data(out);
    int udf_ref=LUA_NOREF;
    if(nr>0)udf_ref=resolve_udf_chunk(L,duckdb_data_chunk_get_vector(in,0));
    if(udf_ref==LUA_NOREF){invalidate_all(out,nr);goto lua_cleanup;}
    for(idx_t r=0;r<nr;r++){
        lua_rawgeti(L,LUA_REGISTRYINDEX,udf_ref);
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
            else if(dt==DUCKDB_TYPE_DATE)
                push_date(L,v,r);
            else if(dt==DUCKDB_TYPE_TIMESTAMP||dt==DUCKDB_TYPE_TIMESTAMP_S||dt==DUCKDB_TYPE_TIMESTAMP_MS||dt==DUCKDB_TYPE_TIMESTAMP_NS)
                push_timestamp(L,v,r);
            else if(dt==DUCKDB_TYPE_DECIMAL)
                push_decimal(L,v,r);
            else if(dt==DUCKDB_TYPE_HUGEINT||dt==DUCKDB_TYPE_UHUGEINT)
                push_hugeint(L,v,r);
            else
                push_str(L,v,r);
        }
        if(!call_udf(fi,L,(int)(nc-1),1)){invalidate_from(out,r,nr);break;}
        if(lua_isnil(L,-1)||!lua_isnumber(L,-1))
            mark_invalid(out,r);
        else od[r]=lua_tonumber(L,-1);
        lua_pop(L,1);
    }
    luaL_unref(L,LUA_REGISTRYINDEX,udf_ref);
    LUA_CLEANUP();
}

/* ── LIST bridge ── */

typedef struct { uint64_t offset; uint64_t length; } list_entry_t;

static void push_list_to_lua(lua_State *L, duckdb_vector list_vec, duckdb_vector child_vec, idx_t r) {
    list_entry_t *entries = (list_entry_t *)duckdb_vector_get_data(list_vec);
    uint64_t off = entries[r].offset, len = entries[r].length;
    duckdb_type ct = duckdb_get_type_id(duckdb_vector_get_column_type(child_vec));
    duckdb_logical_type clt = duckdb_vector_get_column_type(child_vec);
    uint64_t *validity = duckdb_vector_get_validity(child_vec);
    lua_createtable(L, (int)len, 0);
    for (uint64_t i = 0; i < len; i++) {
        idx_t p = off + i;
        void *cd = duckdb_vector_get_data(child_vec);
        if (validity && !duckdb_validity_row_is_valid(validity, p)) {
            lua_pushnil(L);  /* NULL element → nil, not garbage */
        } else if (ct == DUCKDB_TYPE_BIGINT) {
            lua_pushinteger(L, ((int64_t *)cd)[p]);
        } else if (ct == DUCKDB_TYPE_INTEGER) {
            lua_pushinteger(L, ((int32_t *)cd)[p]);
        } else if (ct == DUCKDB_TYPE_SMALLINT) {
            lua_pushinteger(L, ((int16_t *)cd)[p]);
        } else if (ct == DUCKDB_TYPE_TINYINT) {
            lua_pushinteger(L, ((int8_t *)cd)[p]);
        } else if (ct == DUCKDB_TYPE_UBIGINT) {
            lua_pushnumber(L, (lua_Number)((uint64_t *)cd)[p]);
        } else if (ct == DUCKDB_TYPE_UINTEGER) {
            lua_pushinteger(L, ((uint32_t *)cd)[p]);
        } else if (ct == DUCKDB_TYPE_USMALLINT) {
            lua_pushinteger(L, ((uint16_t *)cd)[p]);
        } else if (ct == DUCKDB_TYPE_UTINYINT) {
            lua_pushinteger(L, ((uint8_t *)cd)[p]);
        } else if (ct == DUCKDB_TYPE_DOUBLE) {
            lua_pushnumber(L, ((double *)cd)[p]);
        } else if (ct == DUCKDB_TYPE_FLOAT) {
            lua_pushnumber(L, ((float *)cd)[p]);
        } else if (ct == DUCKDB_TYPE_DECIMAL) {
            push_decimal_any(L, clt, cd, p);
        } else if (ct == DUCKDB_TYPE_BOOLEAN) {
            lua_pushboolean(L, ((bool *)duckdb_vector_get_data(child_vec))[p]);
        } else if (ct == DUCKDB_TYPE_DATE) {
            duckdb_date_struct ds = duckdb_from_date(((duckdb_date *)duckdb_vector_get_data(child_vec))[p]);
            char buf[16]; snprintf(buf, sizeof(buf), "%04d-%02d-%02d", ds.year, ds.month, ds.day);
            lua_pushstring(L, buf);
        } else if (ct == DUCKDB_TYPE_TIMESTAMP || ct == DUCKDB_TYPE_TIMESTAMP_S ||
                   ct == DUCKDB_TYPE_TIMESTAMP_MS || ct == DUCKDB_TYPE_TIMESTAMP_NS) {
            duckdb_type tct = ct;
            int64_t us;
            if (tct == DUCKDB_TYPE_TIMESTAMP_S)      us = ((duckdb_timestamp_s *)duckdb_vector_get_data(child_vec))[p].seconds * 1000000;
            else if (tct == DUCKDB_TYPE_TIMESTAMP_MS) us = ((int64_t *)duckdb_vector_get_data(child_vec))[p] * 1000;
            else if (tct == DUCKDB_TYPE_TIMESTAMP_NS) us = ((int64_t *)duckdb_vector_get_data(child_vec))[p] / 1000;
            else                                      us = ((duckdb_timestamp *)duckdb_vector_get_data(child_vec))[p].micros;
            push_timestamp_micros(L, us);
        } else if (ct == DUCKDB_TYPE_DECIMAL) {
            push_decimal_any(L, duckdb_vector_get_column_type(child_vec), duckdb_vector_get_data(child_vec), p);
        } else if (ct == DUCKDB_TYPE_HUGEINT || ct == DUCKDB_TYPE_UHUGEINT) {
            lua_pushnumber(L, duckdb_hugeint_to_double(((duckdb_hugeint *)duckdb_vector_get_data(child_vec))[p]));
        } else if (ct == DUCKDB_TYPE_VARCHAR) {
            duckdb_string_t s = ((duckdb_string_t *)duckdb_vector_get_data(child_vec))[p];
            lua_pushlstring(L, duckdb_string_t_data(&s), duckdb_string_t_length(s));
        } else {
            lua_pushnil(L);
        }
        lua_rawseti(L, -2, (int)(i + 1));
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
    uint64_t *validity = NULL;
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, -1, i + 1); idx_t p = cs + i;
        if (lua_isnil(L, -1)) {
            if (!validity) { duckdb_vector_ensure_validity_writable(child); validity = duckdb_vector_get_validity(child); }
            duckdb_validity_set_row_invalid(validity, p);
        } else if (ct == DUCKDB_TYPE_DOUBLE || ct == DUCKDB_TYPE_FLOAT)
            ((double *)duckdb_vector_get_data(child))[p] = lua_tonumber(L, -1);
        else if (ct == DUCKDB_TYPE_BIGINT || ct == DUCKDB_TYPE_INTEGER ||
                 ct == DUCKDB_TYPE_SMALLINT || ct == DUCKDB_TYPE_TINYINT)
            ((int64_t *)duckdb_vector_get_data(child))[p] = (int64_t)lua_tointeger(L, -1);
        else if (ct == DUCKDB_TYPE_BOOLEAN)
            ((bool *)duckdb_vector_get_data(child))[p] = lua_toboolean(L, -1);
        else if (ct == DUCKDB_TYPE_VARCHAR)
            duckdb_vector_assign_string_element(child, p, lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
        lua_pop(L, 1);
    }
}

/* Zero the list entry (offset/length) of row r so DuckDB never reads garbage
 * list metadata for a NULL/invalid row — a real crash source for LIST outputs. */
static void zero_list_entry(duckdb_vector out, idx_t r) {
    list_entry_t *e=(list_entry_t*)duckdb_vector_get_data(out);
    e[r].offset=0; e[r].length=0;
}

static void fjl(duckdb_function_info fi, duckdb_data_chunk in, duckdb_vector out) {
    LUA_BEGIN();
    idx_t nr = duckdb_data_chunk_get_size(in), nc = duckdb_data_chunk_get_column_count(in);
    duckdb_vector oc = duckdb_list_vector_get_child(out);
    int udf_ref=LUA_NOREF;
    if(nr>0)udf_ref=resolve_udf_chunk(L,duckdb_data_chunk_get_vector(in,0));
    if(udf_ref==LUA_NOREF){
        invalidate_all(out,nr);
        for(idx_t i=0;i<nr;i++) zero_list_entry(out,i);
        duckdb_list_vector_set_size(out,0);
        goto lua_cleanup;
    }
    for (idx_t r = 0; r < nr; r++) {
        lua_rawgeti(L,LUA_REGISTRYINDEX,udf_ref);
        for (idx_t c = 1; c < nc; c++)
            push_list_to_lua(L, duckdb_data_chunk_get_vector(in, c),
                             duckdb_list_vector_get_child(duckdb_data_chunk_get_vector(in, c)), r);
        if (!call_udf(fi,L,(int)(nc-1),1)){
            for(idx_t rr=r;rr<nr;rr++){mark_invalid(out,rr);zero_list_entry(out,rr);}
            duckdb_list_vector_set_size(out,0);
            break;
        }
        if (!lua_istable(L, -1))
            { mark_invalid(out, r); zero_list_entry(out,r); lua_pop(L, 1); continue; }
        write_lua_to_list(L, out, oc, r);
        lua_pop(L, 1);
    }
    luaL_unref(L,LUA_REGISTRYINDEX,udf_ref);
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
        duckdb_logical_type clt = duckdb_struct_type_child_type(st, i);
        duckdb_type ct = duckdb_get_type_id(clt);
        void *cd = duckdb_vector_get_data(child);
        /* NULL child → nil, not missing key */
        if (!duckdb_validity_row_is_valid(duckdb_vector_get_validity(child), r)) {
            lua_pushnil(L); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_TINYINT) {
            lua_pushinteger(L, ((int8_t *)cd)[r]); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_SMALLINT) {
            lua_pushinteger(L, ((int16_t *)cd)[r]); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_INTEGER) {
            lua_pushinteger(L, ((int32_t *)cd)[r]); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_BIGINT) {
            lua_pushinteger(L, ((int64_t *)cd)[r]); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_UTINYINT) {
            lua_pushinteger(L, ((uint8_t *)cd)[r]); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_USMALLINT) {
            lua_pushinteger(L, ((uint16_t *)cd)[r]); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_UINTEGER) {
            lua_pushinteger(L, ((uint32_t *)cd)[r]); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_UBIGINT) {
            /* uint64 may exceed 2^53; push as double (precision loss acceptable) */
            lua_pushnumber(L, (lua_Number)((uint64_t *)cd)[r]); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_DOUBLE) {
            lua_pushnumber(L, ((double *)cd)[r]); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_FLOAT) {
            lua_pushnumber(L, ((float *)cd)[r]); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_DECIMAL) {
            push_decimal_any(L, clt, cd, r); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_VARCHAR) {
            duckdb_string_t s = ((duckdb_string_t *)cd)[r];
            lua_pushlstring(L, duckdb_string_t_data(&s), duckdb_string_t_length(s));
            lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_BOOLEAN) {
            lua_pushboolean(L, ((bool *)cd)[r]); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_DATE) {
            push_date(L, child, r); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_TIMESTAMP || ct == DUCKDB_TYPE_TIMESTAMP_S ||
                   ct == DUCKDB_TYPE_TIMESTAMP_MS || ct == DUCKDB_TYPE_TIMESTAMP_NS) {
            push_timestamp(L, child, r); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_HUGEINT || ct == DUCKDB_TYPE_UHUGEINT) {
            push_hugeint(L, child, r); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_STRUCT) {
            push_struct_to_lua(L, child, r); lua_setfield(L, -2, name);
        } else if (ct == DUCKDB_TYPE_LIST) {
            push_list_to_lua(L, child, duckdb_list_vector_get_child(child), r);
            lua_setfield(L, -2, name);
        }
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
    int udf_ref=LUA_NOREF;
    if(nr>0)udf_ref=resolve_udf_chunk(L,duckdb_data_chunk_get_vector(in,0));
    if(udf_ref==LUA_NOREF){invalidate_all(out,nr);goto lua_cleanup;}
    for (idx_t r = 0; r < nr; r++) {
        lua_rawgeti(L,LUA_REGISTRYINDEX,udf_ref);
        push_struct_to_lua(L, duckdb_data_chunk_get_vector(in, 1), r);
        if (!call_udf(fi,L,1,1)){invalidate_from(out,r,nr);break;}
        to_str(L);
        duckdb_vector_assign_string_element(out, r, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    luaL_unref(L,LUA_REGISTRYINDEX,udf_ref);
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
    int udf_ref=LUA_NOREF;
    if(nr>0)udf_ref=resolve_udf_chunk(L,duckdb_data_chunk_get_vector(in,0));
    if(udf_ref==LUA_NOREF){invalidate_all(out,nr);goto lua_cleanup;}
    for (idx_t r = 0; r < nr; r++) {
        lua_rawgeti(L,LUA_REGISTRYINDEX,udf_ref);
        push_map_to_lua(L, duckdb_data_chunk_get_vector(in, 1), r);
        if (!call_udf(fi,L,1,1)){invalidate_from(out,r,nr);break;}
        to_str(L);
        duckdb_vector_assign_string_element(out, r, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    luaL_unref(L,LUA_REGISTRYINDEX,udf_ref);
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
        /* Unescape \n \t \\ (save escapes them so the tab-delimited line
         * format survives multi-line sources). */
        {
            char *out = source;
            for (char *in = source; *in; in++) {
                if (in[0] == '\\' && in[1] == 'n') { *out++ = '\n'; in++; }
                else if (in[0] == '\\' && in[1] == 't') { *out++ = '\t'; in++; }
                else if (in[0] == '\\' && in[1] == '\\') { *out++ = '\\'; in++; }
                else *out++ = *in;
            }
            *out = 0;
        }
        if (luaL_loadstring(L, source) != LUA_OK) { lua_pop(L, 1); continue; }
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) { lua_pop(L, 1); continue; }
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
        lua_setglobal(L, name);
        /* P4: register in shared table so other threads can lazy-compile */
        udf_source_set(name, source);
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
    double  *vals; idx_t len, cap;    /* DOUBLE overload */
    int64_t *ivals; idx_t ilen, icap; /* BIGINT overload */
    int is_i64;                        /* set by agg_init from extra_info */
} agg_group_t;

static idx_t agg_state_size(duckdb_function_info fi) { (void)fi; return sizeof(agg_group_t *); }

static void agg_init_common(duckdb_aggregate_state st, int is_i64) {
    agg_group_t *g = (agg_group_t *)malloc(sizeof(agg_group_t));
    memset(g, 0, sizeof(*g));
    g->is_i64 = is_i64;
    *(agg_group_t **)st = g;
}
static void agg_init(duckdb_function_info fi, duckdb_aggregate_state st)      { (void)fi; agg_init_common(st, 0); }
static void agg_init_i64(duckdb_function_info fi, duckdb_aggregate_state st)  { (void)fi; agg_init_common(st, 1); }
static void agg_update(duckdb_function_info fi, duckdb_data_chunk in, duckdb_aggregate_state *states) {
    (void)fi;
    duckdb_vector nv = duckdb_data_chunk_get_vector(in, 0);
    duckdb_string_t ns0 = ((duckdb_string_t *)duckdb_vector_get_data(nv))[0];
    const char *udf_name = duckdb_string_t_data(&ns0);
    idx_t udf_len = duckdb_string_t_length(ns0);

    duckdb_vector cv = duckdb_data_chunk_get_vector(in, 1);
    double *cd = (double *)duckdb_vector_get_data(cv);
    int64_t *ci = (int64_t *)cd;  /* same slot; interpreted per group type */
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
        if (g->is_i64) {
            if (g->ilen + 1 > g->icap) {
                g->icap = g->icap ? g->icap * 2 : 1024;
                g->ivals = (int64_t *)realloc(g->ivals, g->icap * sizeof(int64_t));
            }
            g->ivals[g->ilen++] = ci[i];
        } else {
            if (g->len + 1 > g->cap) {
                g->cap = g->cap ? g->cap * 2 : 1024;
                g->vals = (double *)realloc(g->vals, g->cap * sizeof(double));
            }
            g->vals[g->len++] = cd[i];
        }
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
        if (sg->is_i64) {
            for (idx_t j = 0; j < sg->ilen; j++) {
                if (dg->ilen + 1 > dg->icap) {
                    dg->icap = dg->icap ? dg->icap * 2 : 1024;
                    dg->ivals = (int64_t *)realloc(dg->ivals, dg->icap * sizeof(int64_t));
                }
                dg->ivals[dg->ilen++] = sg->ivals[j];
            }
            free(sg->ivals);
        } else {
            for (idx_t j = 0; j < sg->len; j++) {
                if (dg->len + 1 > dg->cap) {
                    dg->cap = dg->cap ? dg->cap * 2 : 1024;
                    dg->vals = (double *)realloc(dg->vals, dg->cap * sizeof(double));
                }
                dg->vals[dg->len++] = sg->vals[j];
            }
            free(sg->vals);
        }
        free(sg->udf);
        free(sg);
        *(agg_group_t **)s[i] = NULL;
    }
}

static void agg_destroy(duckdb_function_info fi, duckdb_aggregate_state *states, idx_t count) {
    (void)fi;
    for (idx_t i = 0; i < count; i++) {
        agg_group_t *g = *(agg_group_t **)states[i];
        if (g) { free(g->vals); free(g->ivals); free(g->udf); free(g); }
        *(agg_group_t **)states[i] = NULL;
    }
}

static void agg_finalize(duckdb_function_info fi, duckdb_aggregate_state *src, duckdb_vector result, idx_t count, idx_t offset) {
    LUA_LOCK();

    double *od = (double *)duckdb_vector_get_data(result);
    int64_t *od64 = (int64_t *)od;  /* same slot; per-group interpretation */

    L_(); lua_State *L = g_lua;
    if (!L) { LUA_UNLOCK(); return; }

    int udf_ref = LUA_NOREF;
    const char *udf_name = NULL;

    for (idx_t i = 0; i < count; i++) {
        agg_group_t *g = *(agg_group_t **)src[i];
        if (!g || !g->udf ||
            (g->is_i64 ? (!g->ivals || g->ilen == 0) : (!g->vals || g->len == 0))) {
            if (g && g->is_i64) od64[offset + i] = 0; else od[offset + i] = 0.0;
            continue;
        }

        /* (Re)resolve the Lua UDF when the name changes across states */
        if (!udf_name || strcmp(udf_name, g->udf)) {
            if (udf_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, udf_ref);
            udf_name = g->udf;
            lua_getglobal(L, udf_name);
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                /* P4: lazily compile from the shared source table — the UDF
                 * may have been compiled on another thread's state. */
                LUA_LOCK();
                char *src = udf_source_get(udf_name);
                LUA_UNLOCK();
                int ok = 0;
                if (src) {
                    if (luaL_loadstring(L, src) == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK &&
                        lua_isfunction(L, -1)) {
                        lua_pushvalue(L, -1);
                        lua_setglobal(L, udf_name);
                        ok = 1;
                    } else {
                        lua_pop(L, lua_gettop(L));
                    }
                    free(src);
                }
                if (!ok) { udf_ref = LUA_NOREF; if (g->is_i64) od64[offset + i] = 0; else od[offset + i] = 0.0; continue; }
            }
            udf_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* keep UDF alive across calls */
        }

        lua_rawgeti(L, LUA_REGISTRYINDEX, udf_ref);
        if (g->is_i64) {
            lua_createtable(L, (int)g->ilen, 0);
            for (idx_t j = 0; j < g->ilen; j++)
                { lua_pushnumber(L, (lua_Number)g->ivals[j]); lua_rawseti(L, -2, (int)(j + 1)); }
        } else {
            lua_createtable(L, (int)g->len, 0);
            for (idx_t j = 0; j < g->len; j++)
                { lua_pushnumber(L, g->vals[j]); lua_rawseti(L, -2, (int)(j + 1)); }
        }

        if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isnumber(L, -1)) {
            if (g->is_i64) od64[offset + i] = (int64_t)lua_tonumber(L, -1);
            else od[offset + i] = lua_tonumber(L, -1);
        } else {
            if (g->is_i64) od64[offset + i] = 0; else od[offset + i] = 0.0;
        }
        lua_pop(L, 1);
    }
    if (udf_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, udf_ref);
    LUA_UNLOCK();
}

/* ── luajit_table: Lua table function ── */

typedef struct { char *source; } tbt_data_t;
/* table mode: rows[] materialized at init; generator mode: gen_ref is a
 * coroutine.wrap iterator resumed per chunk (streaming, O(1) memory). */
typedef struct {
    int cur_row; char **rows; int nrows;
    int gen_ref;
} tbt_state_t;

static void tbt_state_free(void *p) {
    tbt_state_t *s = (tbt_state_t *)p;
    if (s) {
        if (s->gen_ref != LUA_NOREF && g_lua) {
            LUA_LOCK();
            luaL_unref(g_lua, LUA_REGISTRYINDEX, s->gen_ref);
            LUA_UNLOCK();
        }
        if (s->rows) duckdb_free(s->rows);
        duckdb_free(s);
    }
}

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
    s->gen_ref = LUA_NOREF;
    duckdb_init_set_init_data(info, s, tbt_state_free);

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

        /* Generator mode: UDF returned a function (coroutine.wrap iterator) —
         * stream rows via coroutine.resume per chunk instead of materializing. */
        if (lua_isfunction(L, -1)) {
            s->gen_ref = luaL_ref(L, LUA_REGISTRYINDEX);
            LUA_UNLOCK();
            return;
        }
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
    if (!s) { duckdb_data_chunk_set_size(out, 0); return; }

    /* Generator mode: resume coroutine.wrap iterator — yields (row_idx, val);
     * nil row_idx (or dead coroutine) ends the stream. */
    if (s->gen_ref != LUA_NOREF) {
        LUA_LOCK();
        lua_State *L = g_lua;
        if (!L) { LUA_UNLOCK(); duckdb_data_chunk_set_size(out, 0); return; }
        lua_rawgeti(L, LUA_REGISTRYINDEX, s->gen_ref);
        if (lua_pcall(L, 0, 2, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            char buf[256];
            snprintf(buf, sizeof(buf), "luajit_table generator error: %s", err ? err : "unknown");
            free(g_last_error);
            g_last_error = strdup(buf);
            lua_pop(L, 1);
            LUA_UNLOCK();
            duckdb_data_chunk_set_size(out, 0);
            return;
        }
        if (lua_isnil(L, -2)) {  /* generator exhausted */
            lua_pop(L, 2);
            LUA_UNLOCK();
            duckdb_data_chunk_set_size(out, 0);
            return;
        }
        duckdb_data_chunk_set_size(out, 1);
        ((int64_t *)duckdb_vector_get_data(duckdb_data_chunk_get_vector(out, 0)))[0] = (int64_t)lua_tointeger(L, -2);
        const char *v = lua_tostring(L, -1);
        duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(out, 1), 0, v ? v : "");
        lua_pop(L, 2);
        LUA_UNLOCK();
        return;
    }

    /* table mode: serve materialized rows */
    if (s->cur_row >= s->nrows) { duckdb_data_chunk_set_size(out, 0); return; }
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

    /* ── last_error mode: most recent Lua UDF runtime error ── */
    if(!strcmp(m,"last_error")){
        d->ok=true;d->phase="last_error";
        d->msg=strdup(g_last_error?g_last_error:"(no error)");
        return;
    }

    /* ── trusted mode: toggle sandbox (on/off) ── */
    if(!strcmp(m,"trusted")){
        const char *tsrc = d->source;
        bool on = tsrc && (!strcmp(tsrc,"on")||!strcmp(tsrc,"1")||!strcmp(tsrc,"true"));
        L_();  /* ensure a state exists before applying (P4: TLS) */
        if (on || g_trusted) apply_trusted(g_lua, on);  /* off with no sandbox = no-op */
        d->ok=true; d->phase="trusted";
        d->msg=strdup(on ? "trusted mode ON — io/ffi/package/require/load*/debug removed, os reduced" :
                           "trusted mode OFF — full Lua restored");
        return;
    }

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

    /* ── list mode: enumerate compiled UDFs (P4: shared table) ── */
    if(!strcmp(m,"list")){
        char buf[4096]="";int off=0;
        LUA_LOCK();
        for (int i=0;i<g_udf_count;i++)
            off+=snprintf(buf+off,sizeof(buf)-off,"%s%s",off?", ":"",g_udf_sources[i].name);
        LUA_UNLOCK();
        d->ok=true;d->phase="list";d->msg=strdup("UDFs");d->detail=strdup(buf[0]?buf:"(none)");
        d->sql_name=strdup(buf[0]?buf:"");
        return;
    }

    /* ── drop mode: remove a compiled UDF (P4: shared + current state) ── */
    if(!strcmp(m,"drop")){
        if(!d->sql_name){d->msg=strdup("need sql_name");return;}
        LUA_LOCK();
        bool removed = udf_source_del(d->sql_name);
        LUA_UNLOCK();
        L_();if(g_lua){lua_pushnil(g_lua);lua_setglobal(g_lua,d->sql_name);}
        d->ok=removed;d->phase="drop";d->sql_name=strdup(d->sql_name);
        if(!removed)d->msg=strdup("UDF not found");
        return;
    }

    /* ── reset mode: clear all UDFs (P4: shared table + current state) ── */
    if(!strcmp(m,"reset")){
        LUA_LOCK();
        for (int i=0;i<g_udf_count;i++){free(g_udf_sources[i].name);free(g_udf_sources[i].source);}
        g_udf_count=0;
        LUA_UNLOCK();
        if(g_lua){lua_close(g_lua);g_lua=NULL;}
        L_();
        d->ok=true;d->phase="reset";d->msg=strdup("UDFs reset");
        return;
    }

    /* ── save mode: persist UDFs to file (P4: shared table) ── */
    if(!strcmp(m,"save")){
        const char *path = d->source ? d->source : "luajit_udfs.txt";
        FILE *fp=fopen(path,"w");
        if(!fp){d->msg=strdup("file write failed");return;}

        LUA_LOCK();
        int n_saved=0;
        for(int i=0;i<g_udf_count;i++){
            /* Escape newlines/tabs/backslashes so multi-line sources survive
             * the name<TAB>source line format (load reverses this). */
            fprintf(fp,"%s\t",g_udf_sources[i].name);
            for (const char *p=g_udf_sources[i].source; *p; p++){
                if (*p=='\n') fputs("\\n",fp);
                else if (*p=='\t') fputs("\\t",fp);
                else if (*p=='\\') fputs("\\\\",fp);
                else fputc(*p,fp);
            }
            fputc('\n',fp);
            n_saved++;
        }
        LUA_UNLOCK();
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
        /* P4: register in shared table (other threads compile lazily) */
        udf_source_set(d->sql_name, d->source);
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

    /* ── fennel mode: compile Fennel source → Lua UDF (embedded compiler) ── */
    if(!strcmp(m,"fennel")){
        if(!d->source||!d->sql_name){d->msg=strdup("need source+sql_name");return;}
        if (g_trusted) { d->msg=strdup("fennel unavailable in trusted mode (compiler needs require/package)"); return; }
        L_();lua_State *L=g_lua;
        if(!L){d->msg=strdup("no Lua");return;}
        d->phase="fennel";
        /* Ensure embedded fennel compiler is loaded (cached in registry) */
        lua_getfield(L, LUA_REGISTRYINDEX, "lj_fennel");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            extern unsigned char fennel_lua[];
            extern unsigned int fennel_lua_len;
            /* fennel's CLI shim scans `arg` at top level — provide an empty
             * table so the embedded compiler loads as a library. */
            lua_createtable(L, 0, 0);
            lua_setglobal(L, "arg");
            if (luaL_loadbuffer(L, (const char*)fennel_lua, fennel_lua_len, "fennel") != LUA_OK) {
                char buf[2048];
                snprintf(buf, sizeof(buf), "fennel loadbuffer(%u): %s", fennel_lua_len, lua_tostring(L,-1));
                d->msg=strdup(buf);
                lua_pop(L,1);
                return;
            }
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {   /* registers package.preload entries */
                luaL_traceback(L, L, lua_tostring(L, -1), 1);
                lua_remove(L, -2);
                char buf[2048];
                snprintf(buf, sizeof(buf), "fennel chunk load: %s", lua_tostring(L, -1));
                d->msg=strdup(buf);
                lua_pop(L,1);
                return;
            }
            lua_getglobal(L, "require");
            lua_pushstring(L, "fennel");
            if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
                char buf[1024];
                snprintf(buf, sizeof(buf), "fennel require: %s", lua_tostring(L, -1));
                d->msg=strdup(buf);
                lua_pop(L, lua_gettop(L));
                return;
            }
            lua_setfield(L, LUA_REGISTRYINDEX, "lj_fennel");
            lua_getfield(L, LUA_REGISTRYINDEX, "lj_fennel");
        }
        /* stack: fennel module → compileString(source, {filename=...})
         * NOTE: fennel.compileString is a DOT call (no self) — pushing the
         * module as arg1 makes str = module table → "attempt to call method
         * 'gsub' (a nil value)". Pass exactly (source, opts). */
        lua_getfield(L, -1, "compileString");
        lua_pushstring(L, d->source);
        lua_createtable(L, 0, 1);
        lua_pushstring(L, "udf.fnl");
        lua_setfield(L, -2, "filename");
        if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
            char buf[2048];
            snprintf(buf, sizeof(buf), "fennel compileString: %s", lua_tostring(L,-1));
            d->msg=strdup(buf);
            lua_pop(L,lua_gettop(L));
            return;
        }
        const char *compiled = lua_tostring(L, -1);
        if (!compiled) {d->msg=strdup("fennel: no output");lua_pop(L,2);return;}
        /* Compile the Fennel→Lua output as a UDF */
        if (luaL_loadstring(L, compiled) != LUA_OK) {
            char buf[2048];
            snprintf(buf, sizeof(buf), "fennel output loadstring: %s", lua_tostring(L,-1));
            d->msg=strdup(buf);
            lua_pop(L,2);
            return;
        }
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            char buf[2048];
            snprintf(buf, sizeof(buf), "fennel output exec: %s", lua_tostring(L,-1));
            d->msg=strdup(buf);
            lua_pop(L,2);
            return;
        }
        if (!lua_isfunction(L, -1))
            {d->msg=strdup("fennel: must return a function");lua_pop(L,2);return;}
        lua_setglobal(L, d->sql_name);
        /* Store the COMPILED Lua source (other threads lazy-compile it;
         * save/load round-trips the compiled form) */
        udf_source_set(d->sql_name, compiled);
        d->ok=true;d->sql_name=strdup(d->sql_name);
        d->detail=strdup(compiled);
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
    /* P4: register in shared table (other threads compile lazily) */
    udf_source_set(d->sql_name, d->source);
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
    REG("luajit_vi", fjvi, DUCKDB_TYPE_BIGINT) VARGS(DUCKDB_TYPE_BIGINT) END();
    REG("luajit_vs", fjvs, DUCKDB_TYPE_VARCHAR) VARGS(DUCKDB_TYPE_VARCHAR) END();

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

    /* luajit_agg: aggregate UDF — accumulate values, call Lua on finalize.
     * Function-set overloads: BIGINT in/out (exact integer accumulation,
     * no float coercion) and DOUBLE in/out. Distinct init callbacks carry
     * the i64 flag (agg init's fi has no extra_info). */
    { duckdb_aggregate_function_set fs = duckdb_create_aggregate_function_set("luajit_agg");
      for (int variant = 0; variant < 2; variant++) {
        duckdb_aggregate_function f = duckdb_create_aggregate_function();
        duckdb_aggregate_function_set_name(f, "luajit_agg");
        duckdb_aggregate_function_add_parameter(f, duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
        duckdb_aggregate_function_add_parameter(f,
            duckdb_create_logical_type(variant == 0 ? DUCKDB_TYPE_BIGINT : DUCKDB_TYPE_DOUBLE));
        duckdb_aggregate_function_set_return_type(f,
            duckdb_create_logical_type(variant == 0 ? DUCKDB_TYPE_BIGINT : DUCKDB_TYPE_DOUBLE));
        duckdb_aggregate_function_set_functions(f, agg_state_size,
            variant == 0 ? agg_init_i64 : agg_init,
            agg_update, agg_combine, agg_finalize);
        if (duckdb_add_aggregate_function_to_set(fs, f) == DuckDBError) {
          duckdb_destroy_aggregate_function(&f);
        }
      }
      duckdb_register_aggregate_function_set(conn, fs);
      duckdb_destroy_aggregate_function_set(&fs); }
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
