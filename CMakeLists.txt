# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)

project(slonisko
    VERSION 0.1.0
    DESCRIPTION "PostgreSQL GUI client"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(SLONISKO_BUILD_TESTS "Build unit tests" ON)
option(SLONISKO_WERROR "Treat compiler warnings as errors" OFF)
option(SLONISKO_WITH_KEYCHAIN
    "Store passwords in the system wallet via QtKeychain when it is available" ON)

set(SLONISKO_QT_COMPONENTS Core Network Gui Widgets)
if(SLONISKO_BUILD_TESTS)
    list(APPEND SLONISKO_QT_COMPONENTS Test)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS ${SLONISKO_QT_COMPONENTS})
qt_standard_project_setup(REQUIRES 6.5)

# libpq 17+: chunked rows mode and the non-blocking cancel API.
find_package(PostgreSQL 17 REQUIRED)
include(SloniskoPgQuery)

if(SLONISKO_WITH_KEYCHAIN)
    find_package(Qt6Keychain CONFIG QUIET)
    if(NOT Qt6Keychain_FOUND)
        message(WARNING
            "QtKeychain for Qt 6 not found: passwords will be stored in plain text. "
            "Install it (e.g. qtkeychain-qt6-devel) or pass -DSLONISKO_WITH_KEYCHAIN=OFF.")
    endif()
endif()

include(SloniskoCompilerOptions)

if(SLONISKO_BUILD_TESTS)
    enable_testing()
    include(SloniskoTesting)
endif()

add_subdirectory(src/pg)
add_subdirectory(src/config)
add_subdirectory(src/sql)
add_subdirectory(src/catalog)
add_subdirectory(src/app)
