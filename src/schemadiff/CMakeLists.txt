# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

# Comparing the schemas of two databases, and the script that makes one like
# the other: what the program's Tools → Compare Schemas shows. The model of
# each side is docgen's.
add_library(slonisko_schemadiff STATIC
    Diff.cpp
    Diff.h)
add_library(Slonisko::SchemaDiff ALIAS slonisko_schemadiff)

target_include_directories(slonisko_schemadiff PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(slonisko_schemadiff PUBLIC Qt6::Core Slonisko::Pg Slonisko::Docgen)
slonisko_target_defaults(slonisko_schemadiff)

if(SLONISKO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
