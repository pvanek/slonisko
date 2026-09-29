# Slonisko

A fast, keyboard-driven PostgreSQL client built with Qt6.

Slonisko talks to the server through raw libpq, fully asynchronously, and uses
PostgreSQL's own parser ([libpg_query](https://github.com/pganalyze/libpg_query))
for statement splitting, error positions and context-aware completion.

## Why another PostgreSQL client

I have used DBeaver for years, and it is a fine tool. What I never got used
to is how its editor behaves, how much of the machine it wants, and how it
handles transactions — several of those are my own reports in its bug
tracker, filed as *pvanek*.

For the last few years I work with PostgreSQL and nothing else. So rather
than a tool that speaks every database, I wanted one that speaks this one
properly: quick to start, small while it runs, and still able to do a day's
work.

I have written a database client before: [TOra](https://github.com/tora-tool/tora),
an Oracle client that also talks to PostgreSQL and MySQL. Its codebase is
large and PostgreSQL was never what it was built for, so adding what I
wanted there would have been more work than starting from something that is
PostgreSQL-only from the first line.

That is why slonisko exists.

The name is a joke I liked the sound of: *slonisko* is what the Czech
translations of Winnie-the-Pooh call a Heffalump, the imaginary beast Pooh
and Piglet set out to trap. In Czech it is also plainly a big elephant,
which is about as close to PostgreSQL's own mascot as a name can get.

## How it compares

| | Platforms | Licence | Built on | Footprint | Scope |
|---|---|---|---|---|---|
| **slonisko** | Linux, Windows, macOS | GPL-3.0 | C++, Qt Widgets, libpq | small | PostgreSQL only: everyday work, developer first |
| pgAdmin 4 | Linux, Windows, macOS, web | PostgreSQL licence | Python, web UI | large | PostgreSQL, administration first |
| DBeaver CE | Linux, Windows, macOS | Apache-2.0 | Java, Eclipse RCP | large | Many databases, very broad |
| DataGrip | Linux, Windows, macOS | proprietary | Java, IntelliJ | large | Many databases, very broad |
| Beekeeper Studio CE | Linux, Windows, macOS | GPL-3.0 | TypeScript, Electron | medium | Several databases, everyday work |
| TablePlus | macOS, Windows, Linux | proprietary | native | small | Many databases, everyday work |
| psql, pgcli | anywhere with a terminal | PostgreSQL licence, BSD | C, Python | tiny | PostgreSQL, scripting and the shell |

Footprint is a rough impression of start-up time and memory while idle, not
a benchmark; scope is what each tool sets out to do rather than how well it
does it. Corrections are welcome — the point of the table is to say where
slonisko fits, not to score anyone.

## LLM/AI Note

I created this tool as a pet project by hand. Mostly. Some functionality is created
by LLM though. All code has been reviewed by a human.
I added `CLAUDE.md` instructions for further patches.

## Requirements

- CMake 3.25+
- A C++20 compiler
- Qt 6.5+ (Core, Widgets, Test)
- libpq 17+ (client library only; any supported server version works)
- libpg_query
- [QScintilla](https://riverbankcomputing.com/software/qscintilla) for Qt 6. When no
  system copy is found (or with `-DSLONISKO_BUNDLED_QSCINTILLA=ON`), CMake downloads
  and builds a pinned copy, as on Windows and macOS.
- Optional: [QtKeychain](https://github.com/frankosterfeld/qtkeychain) for Qt 6, to keep
  passwords in the system wallet (Secret Service, KWallet, macOS Keychain, Windows
  Credential Manager). Without it they are stored in plain text in the settings file.
- Optional at run time: the OpenSSH `ssh` client, for SSH tunnels

On openSUSE Tumbleweed:

```sh
sudo zypper install cmake gcc-c++ qt6-base-devel postgresql17-devel qtkeychain-qt6-devel qscintilla-qt6-devel
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

## Layout

| Directory       | Target              | Purpose                                                  |
|-----------------|---------------------|----------------------------------------------------------|
| `src/pg`        | `Slonisko::Pg`      | Async connection, queries, cancel, query queue, row store, SSH tunnel |
| `src/config`    | `Slonisko::Config`  | Connection profiles, passwords (system wallet)           |
| `src/sql`       | `Slonisko::Sql`     | PostgreSQL lexer, statement splitting, psql variables, other languages' bodies |
| `src/catalog`   | `Slonisko::Catalog` | Browser and monitoring queries, catalog snapshots, completion, semantic highlighting, EXPLAIN plans, result editing |
| `src/app`       | `slonisko`          | The GUI                                                  |

Each library keeps its Qt Test unit tests in its own `tests/` subdirectory.

Only `src/app` may depend on Qt Widgets. The libraries stay headless so they
can be tested without a display.

## Documentation

The manual lives in [`docs/`](docs/index.md): using the program, and how it
is put together. From the same sources it builds into a website and into a
Qt help file, which the program shows itself — Help → Manual, or F1 on the
page you are looking at.

```sh
cmake -S . -B build -DSLONISKO_WITH_DOCS=ON
cmake --build build --target docs           # build/docs/html and slonisko.qch
```

Building it needs Sphinx with the MyST parser; the help file also needs
`qhelpgenerator` from Qt's tools, and showing it in the program needs Qt's
Help module. See [building](docs/development/building.md) for the details.

Start with [getting started](docs/getting-started.md),
[connections and transactions](docs/connections.md) or the
[coding style](docs/development/coding-style.md).

## License

Slonisko is free software, licensed under the GNU General Public License,
version 3 or (at your option) any later version. See [LICENSE](LICENSE).
