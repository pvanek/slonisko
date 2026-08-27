# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

# Provides the PgQuery::PgQuery target, either from a system libpg_query or
# from a copy downloaded and built as part of this project.

option(SLONISKO_BUNDLED_PGQUERY
    "Download and build libpg_query instead of using a system copy" OFF)

if(NOT SLONISKO_BUNDLED_PGQUERY)
    find_package(PgQuery)
    if(NOT PgQuery_FOUND)
        message(FATAL_ERROR
            "libpg_query not found. Set PgQuery_ROOT to its checkout or install "
            "prefix, or configure with -DSLONISKO_BUNDLED_PGQUERY=ON to download "
            "and build it automatically.")
    endif()
    return()
endif()

# The URL hash pins the exact tarball; update both together.
include(FetchContent)
FetchContent_Declare(libpg_query
    URL https://github.com/pganalyze/libpg_query/archive/refs/tags/18.0.0.tar.gz
    URL_HASH SHA256=6ad7783f272acfd116455c66a03298a0cac9a9168281df547969219112f0260f
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(libpg_query)
message(STATUS "Using bundled libpg_query 18.0.0")

enable_language(C)
find_package(Threads REQUIRED)

# Mirrors SRC_FILES from libpg_query's Makefile.
set(_pgq "${libpg_query_SOURCE_DIR}")
file(GLOB _pgq_sources CONFIGURE_DEPENDS "${_pgq}/src/*.c" "${_pgq}/src/postgres/*.c")
add_library(slonisko_pg_query STATIC
    ${_pgq_sources}
    "${_pgq}/vendor/protobuf-c/protobuf-c.c"
    "${_pgq}/vendor/xxhash/xxhash.c"
    "${_pgq}/protobuf/pg_query.pb-c.c")
add_library(PgQuery::PgQuery ALIAS slonisko_pg_query)

target_include_directories(slonisko_pg_query
    PUBLIC "${_pgq}"
    PRIVATE "${_pgq}/vendor" "${_pgq}/src/include" "${_pgq}/src/postgres/include")
if(WIN32)
    target_include_directories(slonisko_pg_query PRIVATE "${_pgq}/src/postgres/include/port/win32")
endif()
if(MSVC)
    target_include_directories(slonisko_pg_query PRIVATE "${_pgq}/src/postgres/include/port/win32_msvc")
    target_compile_options(slonisko_pg_query PRIVATE /w)
else()
    # Third-party code: keep its warnings out of our build log.
    target_compile_options(slonisko_pg_query PRIVATE -w -fno-strict-aliasing -fwrapv)
endif()
target_link_libraries(slonisko_pg_query PRIVATE Threads::Threads)
set_target_properties(slonisko_pg_query PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

unset(_pgq)
unset(_pgq_sources)
