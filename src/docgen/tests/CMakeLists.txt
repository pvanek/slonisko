# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

slonisko_add_test(tst_docgen Slonisko::Docgen)
slonisko_add_db_test(tst_docgenmodel Slonisko::Docgen)
# Both run the command line program too.
foreach(test tst_docgen tst_docgenmodel)
    target_compile_definitions(${test} PRIVATE
        SLONISKO_VERSION="${PROJECT_VERSION}"
        SLONISKO_DOCGEN="$<TARGET_FILE:slonisko-docgen>")
    add_dependencies(${test} slonisko-docgen)
endforeach()
