# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Writes slonisko.spec from slonisko.spec.in, with the version from
# CMakeLists.txt, the one place it is kept. Needs nothing but CMake, so the
# spec for OBS can be made without configuring the whole project:
#
#   cmake [-DOUTPUT=<file>] -P tools/opensuse/make-spec.cmake
#
# OUTPUT defaults to build-opensuse/slonisko.spec in the checkout.

cmake_minimum_required(VERSION 3.25)

cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH tools_dir)
cmake_path(GET tools_dir PARENT_PATH source_dir)
if(NOT OUTPUT)
    set(OUTPUT "${source_dir}/build-opensuse/slonisko.spec")
endif()

# project()'s VERSION line; the packaging scripts read it the same way.
file(STRINGS "${source_dir}/CMakeLists.txt" version_line
    REGEX "^ *VERSION [0-9]+\\.[0-9]+\\.[0-9]+$" LIMIT_COUNT 1)
string(STRIP "${version_line}" version_line)
string(REPLACE "VERSION " "" PROJECT_VERSION "${version_line}")
if(NOT PROJECT_VERSION)
    message(FATAL_ERROR "No VERSION line in ${source_dir}/CMakeLists.txt")
endif()

configure_file("${CMAKE_CURRENT_LIST_DIR}/slonisko.spec.in" "${OUTPUT}" @ONLY)
message(STATUS "${OUTPUT}: version ${PROJECT_VERSION}")
