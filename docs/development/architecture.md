# Architecture

```{index} architecture, libraries
```

The program is split so that everything except the widgets can be tested
without a display.

| Directory | What belongs there |
|---|---|
| `src/pg` | libpq: connections, queries, cancelling, the row store, SSH tunnels; running pg_dump, pg_restore and psql |
| `src/config` | Connection profiles and passwords |
| `src/sql` | Lexing, statement splitting, psql syntax, other languages' bodies |
| `src/catalog` | Catalog queries and everything derived from their results |
| `src/docgen` | Schema documentation: its model, pages and writers, and `slonisko-docgen` |
| `src/quality` | The quality report: catalog checks, server health, their analysis and pages, and `slonisko-quality-report` |
| `src/app` | The GUI |

Only `src/app` may depend on Qt Widgets. A query, a parser, a layout or a
renderer belongs in a library where a test can reach it: the ER layout, the
export formats, the CSV import, the completion matcher and the EXPLAIN plan parser all live
outside the widgets and have tests of their own. Each library keeps its
tests in its own `tests/` subdirectory.

## How a statement gets run

1. `sql::splitStatements()` cuts the script into statements and blocks, psql
   meta-commands and `COPY` data included.
2. `sql::PsqlVariables` substitutes `:variables`, remembering where
   everything was so error positions still point at the script.
3. `pg::Connection` sends the statement without blocking and reads the rows
   in chunks, into a `pg::RowStore` — a byte arena per chunk with an offset
   table, so libpq's results can be freed as they arrive.
4. The editor turns each result into messages, rows or a plan, and asks the
   catalog which table the rows came from, so they can be edited.

## Asynchronous all the way

Nothing blocks the UI thread. Connecting, querying and cancelling are
non-blocking libpq calls driven by a socket watcher; `pg::QueryRunner`
queues what is asked of one connection and answers with callbacks. The
catalog snapshot, completion and semantic analysis run off the UI thread;
the snapshot reloads itself when DDL is committed.

## Schema documentation

`src/docgen` reads a `docgen::Model` of the chosen schemas in a few catalog
queries and builds `docgen::Page`s of headings,
tables and code that belong to no format. Each writer — HTML, Markdown,
AsciiDoc, Antora — only spells those pages out, so a new format is one
renderer and the links between pages come out right in all of them.

The program and `slonisko-docgen` share all of it: the program reads the
model through a session's `pg::QueryRunner` and writes on a worker thread,
the command line tool through a connection of its own.

## Quality report

`src/quality` builds on `src/docgen`. It reads the same `docgen::Model`, and
then a `quality::Health` of the server's state and statistics, each part in a
query of its own so that what the account may not read only leaves that
part out. `quality::runChecks()` looks over the model for what is worth
fixing; `quality::analyse()` grades those findings and adds what the
statistics show, as `quality::Topic`s in sections; `quality::buildReport()`
turns those into `docgen::Page`s, which docgen's writers spell out in any
of its formats. The program writes it from the same wizard as the
documentation.
