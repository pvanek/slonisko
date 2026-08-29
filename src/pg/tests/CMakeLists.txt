# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

slonisko_add_db_test(tst_connection Slonisko::Pg Slonisko::Sql Qt6::Network)
slonisko_add_db_test(tst_queryrunner Slonisko::Pg)
slonisko_add_test(tst_sshtunnel Slonisko::Pg)
