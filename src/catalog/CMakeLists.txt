# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(slonisko_catalog STATIC
    Monitoring.cpp
    Monitoring.h
    Objects.cpp
    Objects.h
    Snapshot.cpp
    Snapshot.h)
add_library(Slonisko::Catalog ALIAS slonisko_catalog)

target_include_directories(slonisko_catalog PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(slonisko_catalog PUBLIC Qt6::Core Slonisko::Pg)
slonisko_target_defaults(slonisko_catalog)

if(SLONISKO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
