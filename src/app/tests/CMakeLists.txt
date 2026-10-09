# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

slonisko_add_test(tst_connectiondialog slonisko_ui)
slonisko_add_db_test(tst_browser slonisko_ui)
slonisko_add_test(tst_sqleditor slonisko_ui)
slonisko_add_test(tst_resulttextview slonisko_ui)
slonisko_add_test(tst_resultview slonisko_ui)
slonisko_add_test(tst_exportdialog slonisko_ui)
slonisko_add_test(tst_filebrowser slonisko_ui)
slonisko_add_test(tst_pagewindows slonisko_ui)
slonisko_add_test(tst_shortcuts slonisko_ui)
slonisko_add_test(tst_erdview slonisko_ui)
slonisko_add_db_test(tst_editorpage slonisko_ui)
set_property(TEST tst_connectiondialog tst_browser tst_sqleditor tst_resulttextview tst_resultview tst_exportdialog tst_editorpage tst_filebrowser tst_pagewindows tst_shortcuts tst_erdview APPEND PROPERTY
    ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# Software centres and Flathub reject an invalid desktop or metainfo file,
# and the packaging only finds out after a full build.
find_program(SLONISKO_DESKTOP_FILE_VALIDATE desktop-file-validate)
if(SLONISKO_DESKTOP_FILE_VALIDATE)
    add_test(NAME desktop_file
        COMMAND "${SLONISKO_DESKTOP_FILE_VALIDATE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../cz.yarpen.slonisko.desktop")
endif()
find_program(SLONISKO_APPSTREAMCLI appstreamcli)
if(SLONISKO_APPSTREAMCLI)
    add_test(NAME metainfo_file
        COMMAND "${SLONISKO_APPSTREAMCLI}" validate --no-net
            "${SLONISKO_METAINFO}")
endif()
