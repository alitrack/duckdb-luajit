cmake_minimum_required(VERSION 3.5...3.29)
option(DUCKDB_WASM_EXTENSION "Whether compiling for Wasm target" OFF)

###
# Configuration
###
if(NOT DEFINED EXTENSION_NAME)
    message(FATAL_ERROR "DuckDB extension name is required")
endif()
if (DEFINED TARGET_DUCKDB_VERSION_MAJOR)
    add_definitions(-DDUCKDB_EXTENSION_API_VERSION_MAJOR=${TARGET_DUCKDB_VERSION_MAJOR})
endif()
if (DEFINED TARGET_DUCKDB_VERSION_MINOR)
    add_definitions(-DDUCKDB_EXTENSION_API_VERSION_MINOR=${TARGET_DUCKDB_VERSION_MINOR})
endif()
if (DEFINED TARGET_DUCKDB_VERSION_PATCH)
    add_definitions(-DDUCKDB_EXTENSION_API_VERSION_PATCH=${TARGET_DUCKDB_VERSION_PATCH})
endif()

add_definitions(-DDUCKDB_EXTENSION_NAME=${EXTENSION_NAME})
# Suppress incompatible-pointer-types on GCC/Clang (ARM64 ABI mismatch)
if(NOT MSVC)
  add_compile_options(-Wno-incompatible-pointer-types)
endif()

if (DEFINED DUCKDB_EXTENSION_API_VERSION_UNSTABLE)
    add_definitions(-DDUCKDB_EXTENSION_API_VERSION_UNSTABLE=${DUCKDB_EXTENSION_API_VERSION_UNSTABLE})
endif()

###
# LuaJIT — built via its own Makefile as an imported static library
# Skip when building for WASM (use PUC Lua or interpreter-mode LuaJIT instead)
###
if(NOT DUCKDB_WASM_EXTENSION)
    set(LUAJIT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/LuaJIT)
    set(LUAJIT_LIB ${LUAJIT_DIR}/src/libluajit.a)

    if(WIN32)
        # Use msvcbuild.bat on Windows (MSVC)
        set(LUAJIT_LIB ${LUAJIT_DIR}/src/lua51.lib)
        add_custom_command(
            OUTPUT ${LUAJIT_LIB}
            COMMAND msvcbuild.bat static
            COMMENT "Building LuaJIT (MSVC)..."
            WORKING_DIRECTORY ${LUAJIT_DIR}/src
        )
    else()
        include(ProcessorCount)
        ProcessorCount(NPROC)
        if(NPROC EQUAL 0)
            set(NPROC 4)
        endif()

        if(APPLE)
            set(LUAJIT_MAKE_ENV MACOSX_DEPLOYMENT_TARGET=14.0)
        else()
            set(LUAJIT_MAKE_ENV "")
        endif()

        add_custom_command(
            OUTPUT ${LUAJIT_LIB}
            COMMAND ${CMAKE_COMMAND} -E env ${LUAJIT_MAKE_ENV} make -C ${LUAJIT_DIR} -j${NPROC} BUILDMODE=static XCFLAGS=-fPIC
            COMMENT "Building LuaJIT..."
            WORKING_DIRECTORY ${LUAJIT_DIR}
        )
    endif()

    add_custom_target(luajit_runtime ALL DEPENDS ${LUAJIT_LIB})

    add_library(luajit_imported STATIC IMPORTED)
    set_target_properties(luajit_imported PROPERTIES
        IMPORTED_LOCATION ${LUAJIT_LIB}
    )
    target_include_directories(luajit_imported INTERFACE ${LUAJIT_DIR}/src)
    target_link_libraries(luajit_imported INTERFACE dl m)
    add_dependencies(luajit_imported luajit_runtime)
endif()

###
# Build Extension
###
project(${EXTENSION_NAME} LANGUAGES C)

set(EXTENSION_SOURCES
        src/luajit_entry.c
        src/luajit_module.c
)

if (DUCKDB_WASM_EXTENSION)
    add_library(${EXTENSION_NAME} STATIC ${EXTENSION_SOURCES})
    target_compile_definitions(${EXTENSION_NAME} PRIVATE LUAJIT_WASM_STUB)
else()
    add_library(${EXTENSION_NAME} SHARED ${EXTENSION_SOURCES})
endif()

# Hide symbols
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(VISIBILITY_INLINES_HIDDEN ON)

target_include_directories(${EXTENSION_NAME} PRIVATE src/include)
target_include_directories(${EXTENSION_NAME} PRIVATE duckdb_capi)

if(NOT DUCKDB_WASM_EXTENSION)
    target_include_directories(${EXTENSION_NAME} PRIVATE ${LUAJIT_DIR}/src)
    target_link_libraries(${EXTENSION_NAME} PRIVATE luajit_imported)
    add_dependencies(${EXTENSION_NAME} luajit_runtime)
endif()
