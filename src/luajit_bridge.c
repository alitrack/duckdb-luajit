/*
 * luajit — DuckDB LuaJIT UDF Extension
 * SPDX-License-Identifier: MIT
 *
 * luajit_bridge.c: Runtime bridge between DuckDB vectors and LuaJIT.
 *
 * Strategy:
 * - chunk_scalar_loop: DuckDB delivers a data chunk, we iterate rows,
 *   marshal each row's scalar values onto the Lua stack, call the
 *   user's Lua function, and write the return value back to the output vector.
 * - For v0.1, only scalar types (i64, f64, varchar, bool) are supported.
 *   Composite types (struct, list, map, union) come later.
 * - LuaJIT FFI direct vector access is the planned optimization path
 *   (eliminates per-row marshaling overhead).
 */

#include "duckdb_extension.h"
#include "luajit_module.h"

DUCKDB_EXTENSION_EXTERN

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* ===== Lua stack <-> DuckDB value marshaling ===== */

/* Push a DuckDB scalar value onto the Lua stack based on logical type. */
static void luajit_push_vector_value(
    duckdb_vector vector, idx_t row_idx, duckdb_logical_type type,
    lua_State *L)
{
    switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_BOOLEAN: {
        bool val = ((bool *)duckdb_vector_get_data(vector))[row_idx];
        if (!duckdb_validity_row_is_valid(
                duckdb_vector_get_validity(vector), row_idx))
            lua_pushnil(L);
        else
            lua_pushboolean(L, val);
        break;
    }
    case DUCKDB_TYPE_TINYINT: {
        int8_t val = ((int8_t *)duckdb_vector_get_data(vector))[row_idx];
        if (!duckdb_validity_row_is_valid(
                duckdb_vector_get_validity(vector), row_idx))
            lua_pushnil(L);
        else
            lua_pushinteger(L, (lua_Integer)val);
        break;
    }
    case DUCKDB_TYPE_SMALLINT: {
        int16_t val = ((int16_t *)duckdb_vector_get_data(vector))[row_idx];
        if (!duckdb_validity_row_is_valid(
                duckdb_vector_get_validity(vector), row_idx))
            lua_pushnil(L);
        else
            lua_pushinteger(L, (lua_Integer)val);
        break;
    }
    case DUCKDB_TYPE_INTEGER: {
        int32_t val = ((int32_t *)duckdb_vector_get_data(vector))[row_idx];
        if (!duckdb_validity_row_is_valid(
                duckdb_vector_get_validity(vector), row_idx))
            lua_pushnil(L);
        else
            lua_pushinteger(L, (lua_Integer)val);
        break;
    }
    case DUCKDB_TYPE_BIGINT: {
        int64_t val = ((int64_t *)duckdb_vector_get_data(vector))[row_idx];
        if (!duckdb_validity_row_is_valid(
                duckdb_vector_get_validity(vector), row_idx))
            lua_pushnil(L);
        else
            lua_pushinteger(L, (lua_Integer)val);
        break;
    }
    case DUCKDB_TYPE_FLOAT: {
        float val = ((float *)duckdb_vector_get_data(vector))[row_idx];
        if (!duckdb_validity_row_is_valid(
                duckdb_vector_get_validity(vector), row_idx))
            lua_pushnil(L);
        else
            lua_pushnumber(L, (lua_Number)val);
        break;
    }
    case DUCKDB_TYPE_DOUBLE: {
        double val = ((double *)duckdb_vector_get_data(vector))[row_idx];
        if (!duckdb_validity_row_is_valid(
                duckdb_vector_get_validity(vector), row_idx))
            lua_pushnil(L);
        else
            lua_pushnumber(L, val);
        break;
    }
    case DUCKDB_TYPE_VARCHAR: {
        duckdb_string_t val =
            ((duckdb_string_t *)duckdb_vector_get_data(vector))[row_idx];
        if (!duckdb_validity_row_is_valid(
                duckdb_vector_get_validity(vector), row_idx)) {
            lua_pushnil(L);
        } else {
            lua_pushlstring(L, duckdb_string_t_data(&val), duckdb_string_t_length(val));
        }
        break;
    }
    default:
        lua_pushnil(L);
        break;
    }
}

/* Write a Lua stack value back into a DuckDB result vector. */
static void luajit_write_vector_value(
    duckdb_vector vector, idx_t row_idx, lua_State *L, int lua_idx,
    duckdb_logical_type type)
{
    switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_BOOLEAN: {
        if (lua_isnil(L, lua_idx)) {
            duckdb_validity_set_row_invalid(
                duckdb_vector_get_validity(vector), row_idx);
        } else {
            bool val = (bool)lua_toboolean(L, lua_idx);
            ((bool *)duckdb_vector_get_data(vector))[row_idx] = val;
        }
        break;
    }
    case DUCKDB_TYPE_BIGINT:
    case DUCKDB_TYPE_INTEGER:
    case DUCKDB_TYPE_SMALLINT:
    case DUCKDB_TYPE_TINYINT: {
        if (lua_isnil(L, lua_idx)) {
            duckdb_validity_set_row_invalid(
                duckdb_vector_get_validity(vector), row_idx);
        } else {
            int64_t val = (int64_t)lua_tointeger(L, lua_idx);
            switch (duckdb_get_type_id(type)) {
            case DUCKDB_TYPE_TINYINT:
                ((int8_t *)duckdb_vector_get_data(vector))[row_idx] =
                    (int8_t)val;
                break;
            case DUCKDB_TYPE_SMALLINT:
                ((int16_t *)duckdb_vector_get_data(vector))[row_idx] =
                    (int16_t)val;
                break;
            case DUCKDB_TYPE_INTEGER:
                ((int32_t *)duckdb_vector_get_data(vector))[row_idx] =
                    (int32_t)val;
                break;
            default:
                ((int64_t *)duckdb_vector_get_data(vector))[row_idx] = val;
                break;
            }
        }
        break;
    }
    case DUCKDB_TYPE_FLOAT:
    case DUCKDB_TYPE_DOUBLE: {
        if (lua_isnil(L, lua_idx)) {
            duckdb_validity_set_row_invalid(
                duckdb_vector_get_validity(vector), row_idx);
        } else {
            double val = lua_tonumber(L, lua_idx);
            if (duckdb_get_type_id(type) == DUCKDB_TYPE_FLOAT)
                ((float *)duckdb_vector_get_data(vector))[row_idx] =
                    (float)val;
            else
                ((double *)duckdb_vector_get_data(vector))[row_idx] = val;
        }
        break;
    }
    case DUCKDB_TYPE_VARCHAR: {
        if (lua_isnil(L, lua_idx)) {
            duckdb_validity_set_row_invalid(
                duckdb_vector_get_validity(vector), row_idx);
        } else {
            size_t len;
            const char *str = lua_tolstring(L, lua_idx, &len);
            duckdb_vector_ensure_validity_writable(vector);
            duckdb_vector_assign_string_element(vector, row_idx, str);
        }
        break;
    }
    default:
        duckdb_validity_set_row_invalid(
            duckdb_vector_get_validity(vector), row_idx);
        break;
    }
}

/* ===== UDF execution callback ===== */

/*
 * Chunk-level scalar UDF executor.
 * For each row in the chunk: push input values onto Lua stack,
 * call the user's Lua function, write the return value back.
 */
void luajit_execute_chunk_scalar(
    duckdb_function_info info, duckdb_data_chunk input,
    duckdb_vector output)
{
    /* Retrieve Lua state and function reference from extra_info */
    luajit_udf_extra_t *extra =
        (luajit_udf_extra_t *)duckdb_scalar_function_get_extra_info(info);
    lua_State *L = extra->L;
    int func_ref = extra->func_ref;

    idx_t row_count = duckdb_data_chunk_get_size(input);
    idx_t col_count = duckdb_data_chunk_get_column_count(input);

    /* Push function onto stack once */
    lua_rawgeti(L, LUA_REGISTRYINDEX, func_ref);

    duckdb_logical_type return_type = extra->return_type;

    for (idx_t row = 0; row < row_count; row++) {
        /* Push the function again (it gets consumed by pcall) */
        lua_pushvalue(L, -1); /* copy function */

        /* Push arguments */
        for (idx_t col = 0; col < col_count; col++) {
            duckdb_vector vec = duckdb_data_chunk_get_vector(input, col);
            duckdb_logical_type arg_type =
                extra->arg_types ? extra->arg_types[col] : NULL;
            if (arg_type) {
                luajit_push_vector_value(vec, row, arg_type, L);
            } else {
                lua_pushnil(L);
            }
        }

        /* Call: function(arg1, arg2, ...) */
        int status = lua_pcall(L, (int)col_count, 1, 0);
        if (status != LUA_OK) {
            /* On error: set result to NULL and clear error */
            duckdb_validity_set_row_invalid(
                duckdb_vector_get_validity(output), row);
            lua_pop(L, 1); /* pop error message */
            continue;
        }

        /* Write return value */
        luajit_write_vector_value(output, row, L, -1, return_type);
        lua_pop(L, 1); /* pop return value */
    }

    /* Pop the cached function reference */
    lua_pop(L, 1);
}
