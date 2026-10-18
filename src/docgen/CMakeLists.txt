# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

# Schema documentation: a library the program's Database menu uses, and
# slonisko-docgen, the same thing on the command line.
add_library(slonisko_docgen STATIC
    AsciiDoc.cpp
    Console.cpp
    Console.h
    Document.h
    Generator.cpp
    Generator.h
    Html.cpp
    Markdown.cpp
    Model.cpp
    Model.h
    Pages.cpp
    Pages.h
    Sphinx.cpp
    Writer.cpp
    Writer.h)
add_library(Slonisko::Docgen ALIAS slonisko_docgen)

target_include_directories(slonisko_docgen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(slonisko_docgen
    PUBLIC Qt6::Core Slonisko::Pg Slonisko::Catalog
    PRIVATE Slonisko::Sql)
target_compile_definitions(slonisko_docgen PRIVATE SLONISKO_VERSION="${PROJECT_VERSION}")
slonisko_target_defaults(slonisko_docgen)

qt_add_executable(slonisko-docgen main.cpp)
target_compile_definitions(slonisko-docgen PRIVATE SLONISKO_VERSION="${PROJECT_VERSION}")
target_link_libraries(slonisko-docgen PRIVATE Slonisko::Docgen)
slonisko_target_defaults(slonisko-docgen)
# qt_standard_project_setup() makes every program a GUI one: on Windows that
# would detach it from the console, on macOS make it a bundle.
set_target_properties(slonisko-docgen PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF)

include(GNUInstallDirs)
if(APPLE)
    # Inside the application bundle, the one thing a Mac user installs;
    # macdeployqt points it at the bundle's Qt like the program itself.
    set(docgen_destination slonisko.app/Contents/MacOS)
else()
    set(docgen_destination ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS slonisko-docgen RUNTIME DESTINATION ${docgen_destination})

if(SLONISKO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
