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
    ErdView.cpp
    ErdView.h
    HelpWindow.cpp
    HelpWindow.h
    ObjectPage.cpp
    ObjectPage.h
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
    SvgDevice.cpp
    SvgDevice.h
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
if(Qt6Help_FOUND)
    target_link_libraries(slonisko_ui PUBLIC Qt6::Help)
    target_compile_definitions(slonisko_ui PUBLIC SLONISKO_WITH_HELP)
endif()
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
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_BUNDLE_NAME Slonisko
    MACOSX_BUNDLE_GUI_IDENTIFIER cz.yarpen.slonisko
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_COPYRIGHT "Copyright 2026 Petr Vanek, GPL-3.0-or-later"
    MACOSX_BUNDLE_ICON_FILE slonisko.icns)

if(APPLE)
    # iconutil comes with every Mac, so the .icns is made from the PNGs at
    # build time instead of being one more rendered file in the repository.
    set(iconset "${CMAKE_CURRENT_BINARY_DIR}/slonisko.iconset")
    set(icns "${CMAKE_CURRENT_BINARY_DIR}/slonisko.icns")
    set(iconset_commands COMMAND ${CMAKE_COMMAND} -E make_directory "${iconset}")
    foreach(pair 16:16x16 32:16x16@2x 32:32x32 64:32x32@2x 128:128x128 256:128x128@2x
                 256:256x256 512:256x256@2x 512:512x512)
        string(REPLACE ":" ";" pair "${pair}")
        list(GET pair 0 size)
        list(GET pair 1 name)
        list(APPEND iconset_commands COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/icons/slonisko-${size}.png"
            "${iconset}/icon_${name}.png")
    endforeach()
    add_custom_command(OUTPUT "${icns}"
        ${iconset_commands}
        COMMAND iconutil --convert icns --output "${icns}" "${iconset}"
        DEPENDS ${slonisko_icons}
        COMMENT "Making the macOS application icon"
        VERBATIM)
    target_sources(slonisko PRIVATE "${icns}")
    set_source_files_properties("${icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

include(GNUInstallDirs)
install(TARGETS slonisko
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES cz.yarpen.slonisko.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES cz.yarpen.slonisko.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
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
