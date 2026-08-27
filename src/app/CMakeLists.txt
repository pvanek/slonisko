# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

qt_add_executable(slonisko
    main.cpp
    MainWindow.cpp
    MainWindow.h)

target_compile_definitions(slonisko PRIVATE
    SLONISKO_VERSION="${PROJECT_VERSION}")
target_link_libraries(slonisko PRIVATE
    Qt6::Widgets
    Slonisko::Pg
    Slonisko::Sql
    Slonisko::Catalog)
slonisko_target_defaults(slonisko)

set_target_properties(slonisko PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON)

install(TARGETS slonisko
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
