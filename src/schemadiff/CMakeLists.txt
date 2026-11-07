# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

# Comparing the schemas of two databases, and the script that makes one like
# the other: what the program's Tools → Compare Schemas shows, and
# slonisko-schemadiff, the same thing on the command line. The model of each
# side is docgen's.
add_library(slonisko_schemadiff STATIC
    Diff.cpp
    Diff.h)
add_library(Slonisko::SchemaDiff ALIAS slonisko_schemadiff)

target_include_directories(slonisko_schemadiff PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(slonisko_schemadiff PUBLIC Qt6::Core Slonisko::Pg Slonisko::Docgen)
slonisko_target_defaults(slonisko_schemadiff)

qt_add_executable(slonisko-schemadiff main.cpp)
target_compile_definitions(slonisko-schemadiff PRIVATE SLONISKO_VERSION="${PROJECT_VERSION}")
target_link_libraries(slonisko-schemadiff PRIVATE Slonisko::SchemaDiff)
slonisko_target_defaults(slonisko-schemadiff)
# A console program, as slonisko-docgen is.
set_target_properties(slonisko-schemadiff PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF)

include(GNUInstallDirs)
if(APPLE)
    set(schemadiff_destination slonisko.app/Contents/MacOS)
else()
    set(schemadiff_destination ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS slonisko-schemadiff RUNTIME DESTINATION ${schemadiff_destination})

if(SLONISKO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
