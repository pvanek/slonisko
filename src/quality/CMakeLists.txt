# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

# The database quality report: a library the program's browser uses, and
# slonisko-quality-report, the same thing on the command line. The pages are
# docgen's, and so are the writers.
add_library(slonisko_quality STATIC
    Analysis.cpp
    Analysis.h
    Checks.cpp
    Checks.h
    Health.cpp
    Health.h
    Report.cpp
    Report.h)
add_library(Slonisko::Quality ALIAS slonisko_quality)

target_include_directories(slonisko_quality PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(slonisko_quality PUBLIC Qt6::Core Slonisko::Pg Slonisko::Docgen)
target_compile_definitions(slonisko_quality PRIVATE SLONISKO_VERSION="${PROJECT_VERSION}")
slonisko_target_defaults(slonisko_quality)

qt_add_executable(slonisko-quality-report main.cpp)
target_compile_definitions(slonisko-quality-report PRIVATE SLONISKO_VERSION="${PROJECT_VERSION}")
target_link_libraries(slonisko-quality-report PRIVATE Slonisko::Quality)
slonisko_target_defaults(slonisko-quality-report)
# A console program, as slonisko-docgen is.
set_target_properties(slonisko-quality-report PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF)

include(GNUInstallDirs)
if(APPLE)
    set(quality_destination slonisko.app/Contents/MacOS)
else()
    set(quality_destination ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS slonisko-quality-report RUNTIME DESTINATION ${quality_destination})

if(SLONISKO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
