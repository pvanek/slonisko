# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(slonisko_sql STATIC
    EmbeddedLexer.cpp
    EmbeddedLexer.h
    Keywords.cpp
    Keywords.h
    Lexer.cpp
    Lexer.h
    PsqlVariables.cpp
    PsqlVariables.h
    Splitter.cpp
    Splitter.h)
add_library(Slonisko::Sql ALIAS slonisko_sql)

target_include_directories(slonisko_sql PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(slonisko_sql
    PUBLIC Qt6::Core
    PRIVATE PgQuery::PgQuery)
slonisko_target_defaults(slonisko_sql)

if(SLONISKO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
