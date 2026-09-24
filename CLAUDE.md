# Working on slonisko

A PostgreSQL client in Qt 6 Widgets, talking to the server through raw libpq
and parsing SQL with libpg_query. GPLv3.

## Coding style

The [Qt coding style](https://wiki.qt.io/Qt_Coding_Style), as written down in
`.clang-format`. Run it over anything you touch:

```sh
clang-format -i $(git ls-files '*.cpp' '*.h')
```

- Four spaces, never tabs; lines up to 100 characters.
- Braces on their own line for classes, structs and function bodies,
  attached everywhere else (`if (x) {`, `} else {`).
- `*` and `&` next to the name: `Session *session`, `const QString &name`.
- `m_` for members, `CamelCase` types, `camelCase` functions and variables.
- Single-statement bodies may drop their braces; then no branch of that
  statement has them either.

## What goes where

| Directory     | What belongs there                                             |
|---------------|----------------------------------------------------------------|
| `src/pg`      | libpq: connections, queries, cancelling, the row store, tunnels |
| `src/config`  | Connection profiles and passwords                              |
| `src/sql`     | Lexing, statement splitting, psql syntax                       |
| `src/catalog` | Catalog queries and everything derived from their results      |
| `src/app`     | The GUI                                                        |

Only `src/app` may depend on Qt Widgets. Keep logic out of the widgets: a
query, a parser or a renderer belongs in a library where a test can reach it
without a display. Each library keeps its tests in its own `tests/`.

## House rules

- Every dialog's layout lives in a Qt Designer `.ui` file; the class only
  wires up behaviour.
- Comments explain why, not what. Write one wherever the reason is not
  obvious from the code, and none where it is.
- New behaviour comes with a test. Tests that need a server use the CTest
  fixture, which starts a throwaway `postgres:18` container.
- Run `ctest --test-dir build` before saying anything is done, and say what
  actually happened, failures included.
- Do not commit unless asked to.
