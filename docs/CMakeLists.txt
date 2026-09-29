# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

include(GNUInstallDirs)

find_program(SLONISKO_SPHINX_BUILD NAMES sphinx-build sphinx-build-3)
if(NOT SLONISKO_SPHINX_BUILD)
    message(FATAL_ERROR
        "SLONISKO_WITH_DOCS is on but sphinx-build was not found. "
        "Install Sphinx with the MyST parser, e.g. on openSUSE: "
        "zypper install python313-Sphinx python313-myst-parser python313-furo")
endif()

set(docs_source "${CMAKE_CURRENT_SOURCE_DIR}")
set(docs_binary "${CMAKE_CURRENT_BINARY_DIR}")

# The manual shows the program's icon, and Sphinx takes images only from its
# own directory. The copy is not in the repository: it is made here, so a
# fresh clone needs nothing but the SVG's rendered PNGs.
set(docs_logo "${docs_source}/images/slonisko.png")
add_custom_command(
    OUTPUT "${docs_logo}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/app/icons/slonisko-128.png" "${docs_logo}"
    DEPENDS "${CMAKE_SOURCE_DIR}/src/app/icons/slonisko-128.png"
    COMMENT "Copying the icon into the manual"
    VERBATIM)
add_custom_target(docs-logo DEPENDS "${docs_logo}")

# The website. Sphinx keeps its own cache, so a second run only rebuilds
# the pages that changed.
add_custom_target(docs-html
    COMMAND ${CMAKE_COMMAND} -E env "SLONISKO_VERSION=${PROJECT_VERSION}"
        "${SLONISKO_SPHINX_BUILD}" -b html
        -d "${docs_binary}/cache/html" "${docs_source}" "${docs_binary}/html"
    WORKING_DIRECTORY "${docs_source}"
    COMMENT "Building the manual as a website"
    VERBATIM)
add_dependencies(docs-html docs-logo)

# The help file the program shows. Sphinx writes plain HTML and a .qhp
# project; qhelpgenerator turns those into one .qch, full-text index and all.
find_program(SLONISKO_QHELPGENERATOR
    NAMES qhelpgenerator qhelpgenerator-qt6
    HINTS "${QT6_INSTALL_PREFIX}/libexec" /usr/libexec/qt6 /usr/lib64/qt6/libexec
    PATH_SUFFIXES libexec)

if(SLONISKO_QHELPGENERATOR)
    set(qthelp_dir "${docs_binary}/qthelp")
    # A plain target, run whenever it is asked for: Sphinx is incremental
    # itself, and listing every page as a dependency here would go stale the
    # moment a page is added.
    add_custom_target(docs-qch
        COMMAND ${CMAKE_COMMAND} -E env "SLONISKO_VERSION=${PROJECT_VERSION}"
            "${SLONISKO_SPHINX_BUILD}" -b qthelp -q
            -d "${docs_binary}/cache/qthelp" "${docs_source}" "${qthelp_dir}"
        COMMAND "${SLONISKO_QHELPGENERATOR}" "${qthelp_dir}/slonisko.qhp"
            -o "${docs_binary}/slonisko.qch"
        # Beside the program, so a build directory run finds it too.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${docs_binary}/slonisko.qch"
            "$<TARGET_FILE_DIR:slonisko>/slonisko.qch"
        WORKING_DIRECTORY "${docs_source}"
        COMMENT "Building the manual as a Qt help file"
        VERBATIM)
    add_dependencies(docs-qch docs-logo)

    install(FILES "${docs_binary}/slonisko.qch"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/slonisko"
        OPTIONAL)
else()
    message(STATUS "qhelpgenerator was not found: no help file will be built")
endif()

add_custom_target(docs DEPENDS docs-html)
if(SLONISKO_QHELPGENERATOR)
    add_dependencies(docs docs-qch)
endif()
