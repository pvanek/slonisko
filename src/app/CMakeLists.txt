# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

# Everything but main(), so the tests can use it too.
add_library(slonisko_ui STATIC
    BrowserDelegate.cpp
    BrowserDelegate.h
    BrowserModel.cpp
    BrowserModel.h
    ConnectionBrowser.cpp
    ConnectionBrowser.h
    ConnectionDialog.cpp
    ConnectionDialog.h
    ConnectionDialog.ui
    Icons.cpp
    Icons.h
    MainWindow.cpp
    MainWindow.h
    ResultModel.cpp
    ResultModel.h
    ResultView.cpp
    ResultView.h
    Session.cpp
    Session.h)
target_include_directories(slonisko_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(slonisko_ui PUBLIC
    Qt6::Widgets
    Slonisko::Pg
    Slonisko::Config
    Slonisko::Sql
    Slonisko::Catalog)
slonisko_target_defaults(slonisko_ui)

qt_add_executable(slonisko main.cpp)
target_compile_definitions(slonisko PRIVATE
    SLONISKO_VERSION="${PROJECT_VERSION}")
target_link_libraries(slonisko PRIVATE slonisko_ui)
slonisko_target_defaults(slonisko)

set_target_properties(slonisko PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON)

install(TARGETS slonisko
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(SLONISKO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
