# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(slonisko_pg STATIC
    Connection.cpp
    Connection.h
    Handles.h)
add_library(Slonisko::Pg ALIAS slonisko_pg)

target_include_directories(slonisko_pg PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(slonisko_pg PUBLIC Qt6::Core PostgreSQL::PostgreSQL)
slonisko_target_defaults(slonisko_pg)
