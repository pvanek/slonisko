# Architecture

```{index} architecture, libraries
```

The program is split so that everything except the widgets can be tested
without a display.

| Directory | What belongs there |
|---|---|
| `src/pg` | libpq: connections, queries, cancelling, the row store, SSH tunnels |
| `src/config` | Connection profiles and passwords |
| `src/sql` | Lexing, statement splitting, psql syntax, other languages' bodies |
| `src/catalog` | Catalog queries and everything derived from their results |
| `src/app` | The GUI |

Only `src/app` may depend on Qt Widgets. A query, a parser, a layout or a
renderer belongs in a library where a test can reach it: the ER layout, the
export formats, the completion matcher and the EXPLAIN plan parser all live
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
