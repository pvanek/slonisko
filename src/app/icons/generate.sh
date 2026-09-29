#!/bin/sh
# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Renders the application icon from slonisko.svg into the PNGs, the Windows
# .ico kept next to it, and the manual's copy in docs/images. Needs Inkscape
# and ImageMagick; run it after changing the SVG and commit the results.
set -e
cd "$(dirname "$0")"
for size in 16 22 24 32 48 64 128 256 512; do
    inkscape slonisko.svg --export-type=png --export-width=$size --export-height=$size \
        --export-filename=slonisko-$size.png
done
magick slonisko-16.png slonisko-24.png slonisko-32.png slonisko-48.png slonisko-64.png \
    slonisko-256.png slonisko.ico

# The manual shows the same icon, and Sphinx only takes images that live in
# its own directory.
cp slonisko-128.png ../../../docs/images/slonisko.png
