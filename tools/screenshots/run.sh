#!/bin/sh
# SPDX-FileCopyrightText: 2026 Petr Vanek
# SPDX-License-Identifier: GPL-3.0-or-later

# Retakes the manual's screenshots into docs/images. Needs Docker, Xvfb and
# a build directory configured with -DSLONISKO_SCREENSHOTS=ON:
#
#   tools/screenshots/run.sh [build directory]
#
# Everything runs on a display and a settings directory of its own, so
# neither the desktop nor the real connection profiles are touched.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
source_dir=$(cd "$here/../.." && pwd)
build_dir=$(cd "${1:-$source_dir/build}" && pwd)
image=${SLONISKO_TEST_POSTGRES_IMAGE:-postgres:18}
port=${SLONISKO_SCREENSHOTS_PORT:-55432}
display=${SLONISKO_SCREENSHOTS_DISPLAY:-:97}
container=slonisko-screenshots-$$
work=$(mktemp -d)

cleanup() {
    [ -n "${xvfb:-}" ] && kill "$xvfb" 2>/dev/null || true
    docker rm -f "$container" >/dev/null 2>&1 || true
    rm -rf "$work"
}
trap cleanup EXIT INT TERM

cmake --build "$build_dir" --target slonisko_screenshots

docker run -d --name "$container" -e POSTGRES_HOST_AUTH_METHOD=trust -e POSTGRES_DB=shop \
    -p "127.0.0.1:$port:5432" "$image" >/dev/null
# The image starts a server for its own initialisation first and restarts
# it afterwards; only the second one is the one to talk to.
for _ in $(seq 60); do
    docker logs "$container" 2>&1 | grep -q "init process complete" \
        && docker exec "$container" pg_isready -q -h 127.0.0.1 -U postgres && break
    sleep 1
done
docker exec -i "$container" psql -q -v ON_ERROR_STOP=1 -U postgres -d shop \
    < "$here/shop.sql"

Xvfb "$display" -screen 0 1600x1000x24 -nolisten tcp >/dev/null 2>&1 &
xvfb=$!
sleep 1

# No session bus: nothing may wake a wallet or a desktop portal on the real
# one, and with none the wallet is simply unavailable.
DISPLAY=$display WAYLAND_DISPLAY= QT_QPA_PLATFORM=xcb QT_STYLE_OVERRIDE=Breeze \
    XDG_CONFIG_HOME="$work" DBUS_SESSION_BUS_ADDRESS=disabled: \
    "$build_dir/tools/screenshots/slonisko_screenshots" \
    "host=127.0.0.1 port=$port user=postgres dbname=shop" "$source_dir/docs/images"
