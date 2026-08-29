# Slonisko

A fast, keyboard-driven PostgreSQL client built with Qt 6 Widgets.

Slonisko talks to the server through raw libpq, fully asynchronously, and uses
PostgreSQL's own parser ([libpg_query](https://github.com/pganalyze/libpg_query))
for statement splitting, error positions and context-aware completion.

Status: early development. Nothing works yet.

## Layout

| Directory       | Target              | Purpose                                                  |
|-----------------|---------------------|----------------------------------------------------------|
| `src/pg`        | `Slonisko::Pg`      | Async connection, queries, cancel, query queue, SSH tunnel |
| `src/config`    | `Slonisko::Config`  | Connection profiles, passwords (system wallet)           |
| `src/sql`       | `Slonisko::Sql`     | Statement splitting, parse-tree analysis                 |
| `src/catalog`   | `Slonisko::Catalog` | Object browser and monitoring queries, snapshots         |
| `src/app`       | `slonisko`          | The GUI                                                  |

Each library keeps its Qt Test unit tests in its own `tests/` subdirectory.

Only `src/app` may depend on Qt Widgets. The libraries stay headless so they
can be tested without a display.

## Requirements

- CMake 3.25+
- A C++20 compiler
- Qt 6.5+ (Core, Widgets, Test)
- libpq 17+ (client library only; any supported server version works)
- libpg_query
- Optional: [QtKeychain](https://github.com/frankosterfeld/qtkeychain) for Qt 6, to keep
  passwords in the system wallet (Secret Service, KWallet, macOS Keychain, Windows
  Credential Manager). Without it they are stored in plain text in the settings file.
- Optional at run time: the OpenSSH `ssh` client, for SSH tunnels

On openSUSE Tumbleweed:

```sh
sudo zypper install cmake gcc-c++ qt6-base-devel postgresql17-devel qtkeychain-qt6-devel
```

libpg_query is usually not packaged. Either build it from source and point
CMake at it with `-DPgQuery_ROOT=/path/to/libpg_query`:

```sh
git clone https://github.com/pganalyze/libpg_query.git
make -C libpg_query
```

or let CMake download and build a pinned version as part of the project with
`-DSLONISKO_BUNDLED_PGQUERY=ON`. For offline builds, pass
`-DFETCHCONTENT_SOURCE_DIR_LIBPG_QUERY=/path/to/libpg_query` to use an
already downloaded copy.

## Building

```sh
cmake -S . -B build -DSLONISKO_BUNDLED_PGQUERY=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/src/app/slonisko
```

The database tests (`src/pg`) need a PostgreSQL server. `ctest` starts a
throwaway `postgres:18` container for them with Docker and removes it
afterwards (`-DSLONISKO_TEST_POSTGRES_IMAGE=...` picks another image). Without
Docker they are skipped; to use an existing server instead, set
`SLONISKO_TEST_CONNINFO` to a libpq connection string. The tests create only
temporary objects.

Any CMake generator works; add `-G Ninja` for faster incremental builds if you
have Ninja installed.

## License

Slonisko is free software, licensed under the GNU General Public License,
version 3 or (at your option) any later version. See [LICENSE](LICENSE).
