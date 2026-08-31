# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

slonisko_add_test(tst_connectiondialog slonisko_ui)
slonisko_add_db_test(tst_browser slonisko_ui)
slonisko_add_test(tst_sqleditor slonisko_ui)
slonisko_add_test(tst_filebrowser slonisko_ui)
slonisko_add_db_test(tst_editortab slonisko_ui)
set_property(TEST tst_connectiondialog tst_browser tst_sqleditor tst_editortab tst_filebrowser APPEND PROPERTY
    ENVIRONMENT QT_QPA_PLATFORM=offscreen)
