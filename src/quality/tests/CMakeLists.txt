# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

slonisko_add_test(tst_quality Slonisko::Quality)
slonisko_add_test(tst_checks Slonisko::Quality)
slonisko_add_db_test(tst_qualitydb Slonisko::Quality)
# The database test runs the command line program too.
target_compile_definitions(tst_qualitydb PRIVATE
    SLONISKO_QUALITY_REPORT="$<TARGET_FILE:slonisko-quality-report>")
add_dependencies(tst_qualitydb slonisko-quality-report)
