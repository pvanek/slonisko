# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

#[=======================================================================[.rst:
FindPgQuery
-----------

Finds libpg_query (https://github.com/pganalyze/libpg_query).

Set ``PgQuery_ROOT`` to the libpg_query checkout or install prefix if it is
not in a standard location.

Imported target: ``PgQuery::PgQuery``
#]=======================================================================]

find_path(PgQuery_INCLUDE_DIR
    NAMES pg_query.h
    HINTS "${PgQuery_ROOT}" ENV PgQuery_ROOT
    PATH_SUFFIXES include)

find_library(PgQuery_LIBRARY
    NAMES pg_query libpg_query
    HINTS "${PgQuery_ROOT}" ENV PgQuery_ROOT
    PATH_SUFFIXES lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PgQuery
    REQUIRED_VARS PgQuery_LIBRARY PgQuery_INCLUDE_DIR)

if(PgQuery_FOUND AND NOT TARGET PgQuery::PgQuery)
    add_library(PgQuery::PgQuery UNKNOWN IMPORTED)
    set_target_properties(PgQuery::PgQuery PROPERTIES
        IMPORTED_LOCATION "${PgQuery_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${PgQuery_INCLUDE_DIR}")
endif()

mark_as_advanced(PgQuery_INCLUDE_DIR PgQuery_LIBRARY)
