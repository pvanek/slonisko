# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(slonisko_config STATIC
    ConnectionProfile.cpp
    ConnectionProfile.h
    PasswordStore.cpp
    PasswordStore.h
    ProfileStore.cpp
    ProfileStore.h)
add_library(Slonisko::Config ALIAS slonisko_config)

target_include_directories(slonisko_config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(slonisko_config PUBLIC Qt6::Core)
if(Qt6Keychain_FOUND)
    target_link_libraries(slonisko_config PRIVATE Qt6Keychain::Qt6Keychain)
    target_compile_definitions(slonisko_config PRIVATE SLONISKO_HAVE_KEYCHAIN)
endif()
slonisko_target_defaults(slonisko_config)

if(SLONISKO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
