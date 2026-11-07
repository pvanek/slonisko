# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

slonisko_add_test(tst_schemadiff Slonisko::SchemaDiff)
slonisko_add_db_test(tst_schemadiffdb Slonisko::SchemaDiff Slonisko::Sql)
# The database test runs the command line program too.
target_compile_definitions(tst_schemadiffdb PRIVATE
    SLONISKO_SCHEMADIFF="$<TARGET_FILE:slonisko-schemadiff>")
add_dependencies(tst_schemadiffdb slonisko-schemadiff)
