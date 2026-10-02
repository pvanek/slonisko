# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

# The manual's screenshots; run.sh builds and runs this.
add_executable(slonisko_screenshots screenshots.cpp)
target_link_libraries(slonisko_screenshots PRIVATE slonisko_ui Qt6::Test)
slonisko_target_defaults(slonisko_screenshots)
