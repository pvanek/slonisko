# Slonisko

A fast, keyboard-driven PostgreSQL client built with Qt 6 Widgets.

Slonisko talks to the server through raw libpq, fully asynchronously, and uses
PostgreSQL's own parser ([libpg_query](https://github.com/pganalyze/libpg_query))
for statement splitting, error positions and context-aware completion.

Status: early development. Nothing works yet.

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

## Connections and transactions

Connecting a saved profile opens a *session*: one SSH tunnel, if the profile
has one, shared by all of the session's connections. Each part of the UI then
uses connections as follows.

| Who                            | Connection                               | Transactions |
|--------------------------------|------------------------------------------|--------------|
| SQL editor page                | Its own, one per editor                  | Yours: autocommit until you run `BEGIN` (or press Begin); open until you commit or roll back, by statement or toolbar button |
| Explain Analyze in an editor   | The editor's                             | Wrapped in `BEGIN`/`ROLLBACK`, or a savepoint inside an open transaction, so it changes nothing |
| Saving edited result rows      | The editor's                             | `BEGIN`/`COMMIT`, or a savepoint inside an open transaction; all rows or none |
| Result page (DBA Tools, System Info) | Its own, one per page              | Autocommit; a running query can be stopped |
| Object tree, completion, semantic highlighting | The session's, shared, one per database | Autocommit; its queries run one after another |

So N editors and M result pages on one profile use N + M + 1 connections,
plus one for each further database the object tree browses.

- Editors are isolated from each other and from the tree: what one editor has
  not committed, nothing else sees. That includes completion and highlighting,
  which learn about new tables only once their DDL is committed.
- An open transaction is never lost silently. Closing an editor or the window,
  or switching the editor to another connection, asks whether to commit or
  roll back (a failed transaction can only be rolled back), and a running
  statement is stopped only if you say so. If the commit fails, the editor
  stays open with the error in Messages.
- Disconnecting a profile asks first if any of its editors has a transaction
  open or a statement running; disconnecting rolls those back.

## Coding style

The code follows the [Qt coding style](https://wiki.qt.io/Qt_Coding_Style),
with `.clang-format` in the repository root as its written form:

- Four spaces, never tabs; lines up to 100 characters.
- Braces on their own line for classes, structs and function bodies,
  attached everywhere else (`if (x) {`, `} else {`).
- `*` and `&` next to the name: `Session *session`, `const QString &name`.
- A space after control keywords, none after a function name: `if (ok)`,
  `run(sql)`.
- Braces may be left out for single-statement bodies, but then the whole
  statement, `else` branch included, goes without them.
- Names: `CamelCase` types, `camelCase` functions and variables, `m_` for
  members, `SCREAMING_CASE` only for macros.

Before committing:

```sh
clang-format -i $(git ls-files '*.cpp' '*.h')
```

Beyond the formatter: comments say why something is done, not what the next
line already says; anything that is not obvious from the code gets a
sentence. Qt Designer `.ui` files hold every dialog's layout, so that it can
be changed without touching code. Only `src/app` may use Qt Widgets.

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

## License

Slonisko is free software, licensed under the GNU General Public License,
version 3 or (at your option) any later version. See [LICENSE](LICENSE).
