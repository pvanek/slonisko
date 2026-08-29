# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

# Starts or removes a throwaway PostgreSQL server in Docker for the
# integration tests. Run in script mode by CTest fixtures:
#
#   cmake -DACTION=start|stop -DCONTAINER=<name> -DIMAGE=<image>
#         -DCONNINFO_FILE=<path> -P SloniskoTestServer.cmake
#
# "start" writes a libpq conninfo for the server to CONNINFO_FILE. When
# SLONISKO_TEST_CONNINFO is set, or Docker is not available, it starts
# nothing and the tests use that conninfo or skip themselves.

cmake_minimum_required(VERSION 3.25)

set(password slonisko)
file(REMOVE "${CONNINFO_FILE}")

if(ACTION STREQUAL "stop")
    find_program(DOCKER docker)
    if(DOCKER)
        execute_process(COMMAND "${DOCKER}" rm -f "${CONTAINER}"
            OUTPUT_QUIET ERROR_QUIET)
    endif()
    return()
endif()

if(NOT ACTION STREQUAL "start")
    message(FATAL_ERROR "ACTION must be start or stop")
endif()

if(DEFINED ENV{SLONISKO_TEST_CONNINFO})
    message(STATUS "Using SLONISKO_TEST_CONNINFO; not starting a container")
    return()
endif()

find_program(DOCKER docker)
if(NOT DOCKER)
    message(STATUS "Docker not found; database tests will be skipped")
    return()
endif()
execute_process(COMMAND "${DOCKER}" info RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(NOT rc EQUAL 0)
    message(STATUS "Docker is not usable; database tests will be skipped")
    return()
endif()

execute_process(COMMAND "${DOCKER}" rm -f "${CONTAINER}" OUTPUT_QUIET ERROR_QUIET)
# Publish on a random free port on the loopback interface only.
execute_process(
    COMMAND "${DOCKER}" run -d --rm --name "${CONTAINER}"
        -e POSTGRES_PASSWORD=${password}
        -p 127.0.0.1::5432
        "${IMAGE}"
    RESULT_VARIABLE rc
    ERROR_VARIABLE err
    OUTPUT_QUIET)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "Could not start ${IMAGE}: ${err}")
endif()

execute_process(COMMAND "${DOCKER}" port "${CONTAINER}" 5432/tcp
    OUTPUT_VARIABLE mapping OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "[0-9]+$" port "${mapping}")
if(NOT port)
    message(FATAL_ERROR "Could not find the published port: ${mapping}")
endif()

# The image first runs a temporary server on a Unix socket only while it
# initialises, so wait until the real one accepts TCP connections.
foreach(attempt RANGE 120)
    execute_process(
        COMMAND "${DOCKER}" exec "${CONTAINER}"
            pg_isready -q -h 127.0.0.1 -U postgres
        RESULT_VARIABLE rc)
    if(rc EQUAL 0)
        break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.5)
endforeach()
if(NOT rc EQUAL 0)
    execute_process(COMMAND "${DOCKER}" logs "${CONTAINER}")
    message(FATAL_ERROR "PostgreSQL in ${CONTAINER} did not become ready")
endif()

file(WRITE "${CONNINFO_FILE}"
    "host=127.0.0.1 port=${port} user=postgres password=${password} dbname=postgres")
message(STATUS "Started ${IMAGE} as ${CONTAINER} on 127.0.0.1:${port}")
