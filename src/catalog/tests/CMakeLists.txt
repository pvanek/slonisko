# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

slonisko_add_test(tst_snapshot Slonisko::Catalog)
slonisko_add_db_test(tst_objects Slonisko::Catalog)
slonisko_add_db_test(tst_plan Slonisko::Catalog)
slonisko_add_test(tst_completion Slonisko::Catalog)
slonisko_add_test(tst_semantic Slonisko::Catalog)
slonisko_add_test(tst_editing Slonisko::Catalog)
