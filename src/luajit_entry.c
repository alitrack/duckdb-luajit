/*
 * luajit — DuckDB LuaJIT UDF Extension
 * SPDX-License-Identifier: MIT
 *
 * Extension entrypoint: manages per-database LuaJIT state and registers
 * the luajit_module() table function on each DuckDB connection.
 */

#include "duckdb_extension.h"
#include "luajit_module.h"
#include <stdatomic.h>
#include <string.h>

DUCKDB_EXTENSION_EXTERN

typedef struct {
	duckdb_database database;
	duckdb_connection connection;
	bool module_registered;
} luajit_registry_entry_t;

static luajit_registry_entry_t *g_registry_entries = NULL;
static idx_t g_registry_count = 0;
static idx_t g_registry_capacity = 0;
static atomic_flag g_registry_lock = ATOMIC_FLAG_INIT;

static void luajit_registry_lock(void) {
	while (atomic_flag_test_and_set_explicit(&g_registry_lock, memory_order_acquire)) {}
}

static void luajit_registry_unlock(void) {
	atomic_flag_clear_explicit(&g_registry_lock, memory_order_release);
}

static idx_t luajit_registry_find(duckdb_database database) {
	for (idx_t i = 0; i < g_registry_count; i++) {
		if (g_registry_entries[i].database == database) return i;
	}
	return (idx_t)-1;
}

static bool luajit_registry_reserve(idx_t wanted) {
	if (g_registry_capacity >= wanted) return true;
	idx_t new_capacity = g_registry_capacity == 0 ? 4 : g_registry_capacity * 2;
	while (new_capacity < wanted) new_capacity *= 2;
	luajit_registry_entry_t *new_entries =
		(luajit_registry_entry_t *)duckdb_malloc(sizeof(luajit_registry_entry_t) * new_capacity);
	if (!new_entries) return false;
	memset(new_entries, 0, sizeof(luajit_registry_entry_t) * new_capacity);
	if (g_registry_entries && g_registry_count > 0) {
		memcpy(new_entries, g_registry_entries, sizeof(luajit_registry_entry_t) * g_registry_count);
		duckdb_free(g_registry_entries);
	}
	g_registry_entries = new_entries;
	g_registry_capacity = new_capacity;
	return true;
}

DUCKDB_EXTENSION_ENTRYPOINT_CUSTOM(duckdb_extension_info info, struct duckdb_extension_access *access) {
	duckdb_database database = NULL;
	if (access && info) {
		duckdb_database *db_ptr = access->get_database(info);
		if (db_ptr) database = *db_ptr;
	}
	if (!database) {
		if (access) access->set_error(info, "failed to get database handle");
		return false;
	}

	luajit_registry_lock();
	idx_t idx = luajit_registry_find(database);
	if (idx == (idx_t)-1) {
		if (!luajit_registry_reserve(g_registry_count + 1)) {
			luajit_registry_unlock();
			if (access) access->set_error(info, "failed to allocate registry entry");
			return false;
		}
		idx = g_registry_count++;
		g_registry_entries[idx].database = database;

		duckdb_connection conn;
		if (duckdb_connect(database, &conn) == DuckDBError) {
			luajit_registry_unlock();
			if (access) access->set_error(info, "failed to create persistent connection");
			return false;
		}
		g_registry_entries[idx].connection = conn;
		g_registry_entries[idx].module_registered = false;
	}
	luajit_registry_entry_t *entry = &g_registry_entries[idx];
	luajit_registry_unlock();

	if (!entry->module_registered) {
		luajit_register_module_functions(entry->connection);
		entry->module_registered = true;
	}

	return true;
}
