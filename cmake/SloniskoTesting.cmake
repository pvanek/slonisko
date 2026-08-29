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

# Database tests share one throwaway PostgreSQL server in Docker, started
# before the first of them and removed after the last (CTest fixtures).
set(SLONISKO_TEST_POSTGRES_IMAGE "postgres:18" CACHE STRING
    "Docker image used as the server for database tests")

string(MD5 _slonisko_build_hash "${CMAKE_BINARY_DIR}")
string(SUBSTRING "${_slonisko_build_hash}" 0 8 _slonisko_build_hash)
set(_slonisko_test_server_args
    -DCONTAINER=slonisko-test-${_slonisko_build_hash}
    -DIMAGE=${SLONISKO_TEST_POSTGRES_IMAGE}
    -DCONNINFO_FILE=${CMAKE_BINARY_DIR}/test-server.conninfo
    -P ${CMAKE_CURRENT_LIST_DIR}/SloniskoTestServer.cmake)
unset(_slonisko_build_hash)

add_test(NAME slonisko_test_server_start
    COMMAND ${CMAKE_COMMAND} -DACTION=start ${_slonisko_test_server_args})
add_test(NAME slonisko_test_server_stop
    COMMAND ${CMAKE_COMMAND} -DACTION=stop ${_slonisko_test_server_args})
set_tests_properties(slonisko_test_server_start PROPERTIES
    FIXTURES_SETUP slonisko_test_server TIMEOUT 300)
set_tests_properties(slonisko_test_server_stop PROPERTIES
    FIXTURES_CLEANUP slonisko_test_server)

# Like slonisko_add_test(), for tests that need a PostgreSQL server. They
# find it through SLONISKO_TEST_CONNINFO or the file the fixture writes.
function(slonisko_add_db_test name)
    slonisko_add_test(${name} ${ARGN})
    set_tests_properties(${name} PROPERTIES
        FIXTURES_REQUIRED slonisko_test_server
        ENVIRONMENT "SLONISKO_TEST_CONNINFO_FILE=${CMAKE_BINARY_DIR}/test-server.conninfo"
        RESOURCE_LOCK slonisko_test_server)
endfunction()
