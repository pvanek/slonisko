#!/bin/sh
# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds Slonisko.app with every library it needs inside it, and a disk image
# of it. Run from anywhere on a Mac with Homebrew; see tools/README.md.
#
# Environment:
#   SLONISKO_SIGN_IDENTITY  "Developer ID Application: ..." to sign for
#                           distribution; without it the bundle is signed
#                           ad hoc, which runs only where it was built.
#   SLONISKO_NOTARY_PROFILE a notarytool keychain profile; with it (and an
#                           identity) the disk image is notarised and stapled.
#   SLONISKO_WITH_DOCS      OFF to leave the manual out (default ON).
set -eu

source_dir=$(cd "$(dirname "$0")/../.." && pwd)
work=${SLONISKO_WORK_DIR:-$source_dir/build-macos}
version=$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' "$source_dir/CMakeLists.txt" | head -n 1)
with_docs=${SLONISKO_WITH_DOCS:-ON}

echo "== Dependencies"
brew install cmake ninja qt libpq qtkeychain
brew_prefix=$(brew --prefix)
# Keg-only: Homebrew does not link it into its prefix.
pq_prefix=$(brew --prefix libpq)

if [ "$with_docs" = ON ]; then
    python3 -m venv "$work/venv"
    "$work/venv/bin/pip" install --quiet --upgrade sphinx myst-parser
    export PATH="$work/venv/bin:$PATH"
fi

echo "== Building $version"
# libpg_query and QScintilla are built into the program from pinned
# tarballs, as nothing on macOS provides them.
cmake -S "$source_dir" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$brew_prefix" \
    -DPostgreSQL_ROOT="$pq_prefix" \
    -DSLONISKO_BUNDLED_PGQUERY=ON \
    -DSLONISKO_BUNDLED_QSCINTILLA=ON \
    -DSLONISKO_WITH_KEYCHAIN=ON \
    -DSLONISKO_BUILD_TESTS=OFF \
    -DSLONISKO_WITH_DOCS="$with_docs"
cmake --build "$work/build"
if [ "$with_docs" = ON ]; then
    cmake --build "$work/build" --target docs-qch
fi

rm -rf "$work/stage"
cmake --install "$work/build" --prefix "$work/stage"
app="$work/stage/slonisko.app"

echo "== Bundling libraries"
# Copies the Qt frameworks and plugins, and every other non-system library
# the program reaches (libpq, OpenSSL, Kerberos, QtKeychain), into
# Contents/Frameworks, and points the program at those copies.
macdeployqt "$app" -verbose=1

# Anything still referring to Homebrew would work here and nowhere else.
leaks=$(find "$app/Contents" -type f \( -perm -u+x -o -name '*.dylib' \) -print0 \
    | xargs -0 otool -L 2>/dev/null \
    | grep -E '^[[:space:]]+(/opt/homebrew|/usr/local)/' || true)
if [ -n "$leaks" ]; then
    echo "The bundle still links outside itself:" >&2
    echo "$leaks" >&2
    exit 1
fi

echo "== Signing"
if [ -n "${SLONISKO_SIGN_IDENTITY:-}" ]; then
    codesign --force --deep --timestamp --options runtime \
        --sign "$SLONISKO_SIGN_IDENTITY" "$app"
else
    # macdeployqt rewrites the binaries; Apple silicon refuses to run
    # them unsigned.
    codesign --force --deep --sign - "$app"
fi
codesign --verify --deep --strict "$app"

echo "== Disk image"
dmg="$work/Slonisko-$version.dmg"
image="$work/image"
rm -rf "$image" "$dmg"
mkdir "$image"
cp -R "$app" "$image/Slonisko.app"
ln -s /Applications "$image/Applications"
hdiutil create -volname "Slonisko $version" -srcfolder "$image" -fs HFS+ \
    -format UDZO -ov "$dmg"
rm -rf "$image"

if [ -n "${SLONISKO_SIGN_IDENTITY:-}" ]; then
    codesign --force --timestamp --sign "$SLONISKO_SIGN_IDENTITY" "$dmg"
    if [ -n "${SLONISKO_NOTARY_PROFILE:-}" ]; then
        xcrun notarytool submit "$dmg" --keychain-profile "$SLONISKO_NOTARY_PROFILE" --wait
        xcrun stapler staple "$dmg"
    fi
fi

echo "== Done: $dmg"
