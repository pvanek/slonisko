# Slonisko

A fast, keyboard-driven PostgreSQL client built with Qt 6 Widgets.

Slonisko talks to the server through raw libpq, fully asynchronously, and uses
PostgreSQL's own parser ([libpg_query](https://github.com/pganalyze/libpg_query))
for statement splitting, error positions and context-aware completion.

Status: early development. Nothing works yet.

## Layout

| Directory       | Target              | Purpose                                                  |
|-----------------|---------------------|----------------------------------------------------------|
| `src/pg`        | `Slonisko::Pg`      | libpq RAII handles, async connection, queries, cancel    |
| `src/sql`       | `Slonisko::Sql`     | Lexing, statement splitting, parse-tree analysis         |
| `src/catalog`   | `Slonisko::Catalog` | Catalog loading, immutable snapshots, local cache        |
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

On openSUSE Tumbleweed:

```sh
sudo zypper install cmake gcc-c++ qt6-base-devel postgresql17-devel
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

Any CMake generator works; add `-G Ninja` for faster incremental builds if you
have Ninja installed.

## License

Slonisko is free software, licensed under the GNU General Public License,
version 3 or (at your option) any later version. See [LICENSE](LICENSE).
