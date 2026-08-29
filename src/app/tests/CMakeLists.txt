# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

slonisko_add_test(tst_connectiondialog slonisko_ui)
slonisko_add_db_test(tst_browser slonisko_ui)
set_property(TEST tst_connectiondialog tst_browser APPEND PROPERTY
    ENVIRONMENT QT_QPA_PLATFORM=offscreen)
