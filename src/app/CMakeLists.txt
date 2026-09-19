# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

# Everything but main(), so the tests can use it too.
add_library(slonisko_ui STATIC
    BrowserDelegate.cpp
    BrowserDelegate.h
    BrowserModel.cpp
    BrowserModel.h
    CompletionPopup.cpp
    CompletionPopup.h
    ConnectionBrowser.cpp
    ConnectionBrowser.h
    ConnectionDialog.cpp
    ConnectionDialog.h
    ConnectionDialog.ui
    EditorPage.cpp
    EditorPage.h
    FileBrowser.cpp
    FileBrowser.h
    Icons.cpp
    Icons.h
    MainWindow.cpp
    MainWindow.h
    PageTabWidget.cpp
    PageTabWidget.h
    PageWindow.cpp
    PageWindow.h
    PlanView.cpp
    PlanView.h
    ExportDialog.cpp
    ExportDialog.h
    ExportDialog.ui
    Qsci.h
    ResultTextView.cpp
    ResultTextView.h
    ResultModel.cpp
    ResultModel.h
    ResultPanel.cpp
    ResultPanel.h
    ResultPage.cpp
    ResultPage.h
    ResultView.cpp
    ResultView.h
    SaveChangesDialog.cpp
    SaveChangesDialog.h
    SaveChangesDialog.ui
    Session.cpp
    Session.h
    Shortcuts.h
    SqlEditor.cpp
    SqlEditor.h
    SqlLexer.cpp
    SqlLexer.h
    WorkspacePage.h)
target_include_directories(slonisko_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(slonisko_ui PUBLIC
    Qt6::Widgets
    Qt6::Concurrent
    QScintilla::QScintilla
    Slonisko::Pg
    Slonisko::Config
    Slonisko::Sql
    Slonisko::Catalog)
slonisko_target_defaults(slonisko_ui)

# The application icon: PNGs rendered from icons/slonisko.svg by
# icons/generate.sh, which also makes the Windows .ico.
set(slonisko_sources main.cpp)
if(WIN32)
    list(APPEND slonisko_sources slonisko.rc)
endif()
qt_add_executable(slonisko ${slonisko_sources})
set(slonisko_icons icons/slonisko.svg)
foreach(size 16 22 24 32 48 64 128 256 512)
    list(APPEND slonisko_icons icons/slonisko-${size}.png)
endforeach()
qt_add_resources(slonisko "icons" PREFIX "/" FILES ${slonisko_icons})
target_compile_definitions(slonisko PRIVATE
    SLONISKO_VERSION="${PROJECT_VERSION}")
target_link_libraries(slonisko PRIVATE slonisko_ui)
slonisko_target_defaults(slonisko)

set_target_properties(slonisko PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON)

include(GNUInstallDirs)
install(TARGETS slonisko
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES slonisko.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES icons/slonisko.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    foreach(size 16 22 24 32 48 64 128 256 512)
        install(FILES icons/slonisko-${size}.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
            RENAME slonisko.png)
    endforeach()
endif()

if(SLONISKO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
