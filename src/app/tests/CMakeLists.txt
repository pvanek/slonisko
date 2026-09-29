# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

slonisko_add_test(tst_connectiondialog slonisko_ui)
slonisko_add_db_test(tst_browser slonisko_ui)
slonisko_add_test(tst_sqleditor slonisko_ui)
slonisko_add_test(tst_resulttextview slonisko_ui)
slonisko_add_test(tst_exportdialog slonisko_ui)
slonisko_add_test(tst_filebrowser slonisko_ui)
slonisko_add_test(tst_pagewindows slonisko_ui)
slonisko_add_test(tst_shortcuts slonisko_ui)
slonisko_add_test(tst_erdview slonisko_ui)
slonisko_add_db_test(tst_editorpage slonisko_ui)
set_property(TEST tst_connectiondialog tst_browser tst_sqleditor tst_resulttextview tst_exportdialog tst_editorpage tst_filebrowser tst_pagewindows tst_shortcuts tst_erdview APPEND PROPERTY
    ENVIRONMENT QT_QPA_PLATFORM=offscreen)
