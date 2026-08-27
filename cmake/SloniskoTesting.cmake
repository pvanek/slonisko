# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

# Builds a Qt Test executable from <name>.cpp and registers it with CTest.
# Extra arguments are libraries to link against.
function(slonisko_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE Qt6::Test ${ARGN})
    slonisko_target_defaults(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()
