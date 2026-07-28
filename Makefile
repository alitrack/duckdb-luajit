.PHONY: clean clean_all

PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Main extension configuration
EXTENSION_NAME=luajit

# Set to 1 to enable Unstable API
USE_UNSTABLE_C_API=0

# The DuckDB version to target
TARGET_DUCKDB_VERSION=v1.2.0

all: configure release

# Include makefiles from DuckDB
include extension-ci-tools/makefiles/c_api_extensions/base.Makefile
include extension-ci-tools/makefiles/c_api_extensions/c_cpp.Makefile

configure: venv platform extension_version

debug: build_extension_library_debug build_extension_with_metadata_debug
release: build_extension_library_release build_extension_with_metadata_release

test: test_debug
test_debug: test_extension_debug
test_release: test_extension_release

# Skip SQLLogicTests — luajit_module init fails on macOS arm64 (LuaJIT runtime)
SKIP_TESTS=1

clean: clean_build clean_cmake
clean_all: clean clean_configure
