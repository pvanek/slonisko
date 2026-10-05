# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)

project(slonisko
    VERSION 0.1.0
    DESCRIPTION "PostgreSQL GUI client"
    LANGUAGES CXX)

# The version above is the only one there is: the AppStream metainfo, the RPM
# spec, the packages and the program itself all take it from here. A release
# changes it and this date, the day it is released, for the metainfo.
set(SLONISKO_RELEASE_DATE 2026-10-05)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(SLONISKO_BUILD_TESTS "Build unit tests" ON)
option(SLONISKO_WERROR "Treat compiler warnings as errors" OFF)
option(SLONISKO_WITH_DOCS "Build the manual; needs Sphinx with the MyST parser" ON)
option(SLONISKO_SCREENSHOTS "Build the tool that takes the manual's screenshots" OFF)
option(SLONISKO_WITH_KEYCHAIN
    "Store passwords in the system wallet via QtKeychain when it is available" ON)

set(SLONISKO_QT_COMPONENTS Concurrent Core Network Gui Widgets)
# The manual is shown in the program itself when Qt Help is there; without
# it the Help menu opens the website instead.
find_package(Qt6 6.5 QUIET COMPONENTS Help)
if(Qt6Help_FOUND)
    list(APPEND SLONISKO_QT_COMPONENTS Help)
endif()
if(SLONISKO_BUILD_TESTS OR SLONISKO_SCREENSHOTS)
    list(APPEND SLONISKO_QT_COMPONENTS Test)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS ${SLONISKO_QT_COMPONENTS})
qt_standard_project_setup(REQUIRES 6.5)
# It gives installed programs a RUNPATH of $ORIGIN, for libraries installed
# beside them. There are none, as every library here is linked in
# statically, and distributions refuse such a RUNPATH in their packages.
set(CMAKE_INSTALL_RPATH "")

# libpq 17+: chunked rows mode and the non-blocking cancel API.
find_package(PostgreSQL 17 REQUIRED)
include(SloniskoPgQuery)
include(SloniskoQScintilla)

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

    # The RPM spec is generated from a template: it has to come out with this
    # version, as rpm itself reads it.
    find_program(SLONISKO_RPMSPEC rpmspec)
    if(SLONISKO_RPMSPEC)
        add_test(NAME rpm_spec_make
            COMMAND ${CMAKE_COMMAND} "-DOUTPUT=${CMAKE_BINARY_DIR}/slonisko.spec"
                -P "${CMAKE_SOURCE_DIR}/tools/opensuse/make-spec.cmake")
        add_test(NAME rpm_spec_version
            COMMAND "${SLONISKO_RPMSPEC}" -q --srpm --queryformat "<%{version}>"
                "${CMAKE_BINARY_DIR}/slonisko.spec")
        string(REPLACE "." "\\." version_regex "${PROJECT_VERSION}")
        set_tests_properties(rpm_spec_make PROPERTIES FIXTURES_SETUP rpm_spec)
        set_tests_properties(rpm_spec_version PROPERTIES
            FIXTURES_REQUIRED rpm_spec
            PASS_REGULAR_EXPRESSION "<${version_regex}>")
    endif()
endif()

add_subdirectory(src/pg)
add_subdirectory(src/config)
add_subdirectory(src/sql)
add_subdirectory(src/catalog)
add_subdirectory(src/app)

if(SLONISKO_SCREENSHOTS)
    add_subdirectory(tools/screenshots)
endif()

if(SLONISKO_WITH_DOCS)
    add_subdirectory(docs)
endif()
