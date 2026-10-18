# Quality report

```{index} quality report, slonisko-quality-report, DBA, health check, database health
```

Slonisko writes a report of what is worth fixing in a database: how its
schemas are made, its indexes, what is broken between its objects, what
exists twice, and how the server and the database are doing. Each finding
says why it matters and, where there is one, the SQL that would deal with
it. Like the {doc}`schema documentation <docgen>`, it comes in two forms
that write the same thing:

- **Database Quality Report…** in the program, in the context menu of a
  database in the browser, on its open connection;
- **`slonisko-quality-report`**, a command line tool installed with it, for
  scripts, cron jobs and continuous integration.

It is written in the same formats as the documentation: a static HTML site,
Markdown, AsciiDoc, an Antora component or a Sphinx project.

## What is in it

```{index} severity; quality report
```

Every finding has a severity:

Critical
: Data or availability is at risk now: a sequence about to run out, a
  replication slot filling the disk, `fsync` off, damaged pages.

Warning
: Worth fixing: a foreign key without an index, an unused index, a
  transaction left open.

Notice
: Worth knowing: a name that needs quoting, a table nobody has described.

The **summary** has the database, the server and a count of each severity
in each section, followed by the critical findings and the warnings, the
first fifty of them. **Suggested statements** collects the SQL of every
finding in one script. Read it before running any of it: dropping an index,
ending a session or rolling a prepared transaction back cannot be undone,
and only whoever knows the database can tell whether it is right.

### Schema advice

Tables without a primary key, foreign keys between different types, row
level security without a policy or policies without row level security,
`SECURITY DEFINER` routines without their own `search_path`, sequences of
another type than their column, names that need quoting, column types the
PostgreSQL wiki's [Don't Do This](https://wiki.postgresql.org/wiki/Don%27t_Do_This)
advises against, and objects without a comment. The
[checks of the catalog](#checks-of-the-catalog) describe each.

### Indexes

Invalid indexes, duplicate and redundant ones, and from the statistics:

Unused indexes
: Never scanned since the statistics were reset, at least 1 MB, neither
  unique nor behind a constraint. They are only reported once the
  statistics are a week old: younger ones cannot tell an unused index from
  one used monthly. A standby keeps statistics of its own, so an index only
  read there looks unused on the primary.

Tables read whole more than through an index
: Tables of 10,000 rows or more scanned sequentially more often than
  through any index: often an index missing. Which one, the queries tell;
  `pg_stat_statements` shows the slowest.

Foreign keys without an index
: Last on the page: after what is wrong with the indexes there are, the
  ones to add.

### Broken dependencies

Constraints added `NOT VALID` and never validated, disabled triggers,
sequences nothing uses, and:

Routines that would fail
: PL/pgSQL is only compiled when a routine is first called. When the
  [plpgsql_check](https://github.com/okbob/plpgsql_check) extension is
  installed in the database, the report runs it over every PL/pgSQL
  routine, a trigger function against a table it is a trigger on, and lists
  the errors: a table, column or function that is gone.

Collation version changed
: The operating system's collation is not the one the database's text
  indexes were built with, after an upgrade of the C library or ICU. They
  may not find what they hold. Suggests `REINDEX DATABASE` and `ALTER
  DATABASE … REFRESH COLLATION VERSION` (PostgreSQL 15 and later).

Extensions behind their installed version
: The server has a newer version than the database uses. Suggests `ALTER
  EXTENSION … UPDATE`.

### Duplicates

The same constraint twice on a table, two triggers on the same events
calling the same function, two views with the same query or two routines
with the same body, and names only their case tells apart.

### Server and database health

An overview of the server, its main settings and the largest tables, and:

| Topic | What it finds |
|---|---|
| Server version | A major version past its end of life (critical) or within six months of it. |
| What the report could see | An account that is neither a superuser nor in `pg_monitor`, and parts of the report it could not read. |
| Settings | `fsync`, `full_page_writes`, `autovacuum` or `track_counts` off (critical); `synchronous_commit` off, no data checksums, `shared_buffers` still at 128MB, over 500 connections; changes waiting for a restart. |
| Connections | Over 80 % of `max_connections` in use, less the reserved ones; over 95 % is critical. |
| Long transactions | Sessions idle in a transaction for 5 minutes, or in one for an hour; a day is critical. Suggests `pg_terminate_backend()`. |
| Forgotten prepared transactions | Prepared over 5 minutes ago (critical). Suggests `ROLLBACK PREPARED`. |
| Replication slots | Slots not in use, lost, or keeping over 10 GB of WAL (critical). |
| Transaction ID wraparound | A database or table over a billion transactions old; 1.5 billion is critical. |
| Dead rows | Tables with 10,000 dead rows that are a fifth of all. Suggests `VACUUM`. |
| Tables autovacuum leaves alone | `autovacuum_enabled = false`. |
| Planner statistics out of date | Tables never analyzed, or with a fifth changed since. Suggests `ANALYZE`. |
| Sequences running out | Over 75 % of a sequence used, 90 % critical, counting the limit of the column it belongs to. |
| Cache hit ratio | Under 99 % of blocks read from shared buffers; under 90 % is a warning. |
| Errors and spills | Checksum failures (critical), deadlocks, over 1 GB of temporary files, over a tenth of transactions rolled back. |
| Schemas anyone may create objects in | `PUBLIC` has `CREATE` on a schema. |

What the statistics count, they count since they were last reset, or the
server last started.

### Checks of the catalog

```{index} checks, foreign key index, redundant index, duplicate index
```

These need nothing but the catalog, so they are in the report with
`--no-health` too, and any account can run them.

Two indexes are alike only when they can do the same work: the same
columns or expressions with the same operator classes and collations,
sorting the same way. A btree reads backwards as well as forwards, so
`(x DESC)` is the same as `(x)`, but `(x DESC NULLS LAST)` and `(x, y DESC)`
are not the same as `(x)` and `(x, y)`. An index that INCLUDEs columns
serves index-only scans another one cannot, unless that one has them too.

| Check | What it finds |
|---|---|
| Tables without a primary key | Tables a row cannot be told apart in, that logical replication cannot update. |
| Foreign keys without an index | The referencing side has no index starting with the key's columns, so deleting a referenced row scans the whole table. Suggests `CREATE INDEX`. |
| Foreign keys between different types | `integer` referencing `bigint` and the like. |
| Invalid indexes | Left by a failed `CREATE INDEX CONCURRENTLY`. Suggests `REINDEX`. |
| Duplicate indexes | Another index has the very same key columns, sorting the same way, and INCLUDEs at least what this one does; a unique one is kept. Suggests `DROP INDEX`. |
| Redundant indexes | Another index has the same leading columns and more, and holds what this one INCLUDEs; unique ones are kept. Suggests `DROP INDEX`. |
| Duplicate constraints | The same foreign key, unique or check constraint twice on a table. Suggests dropping the copy. |
| Constraints not validated | Added `NOT VALID` and never validated. Suggests `VALIDATE CONSTRAINT`. |
| Disabled triggers | Triggers switched off with `DISABLE TRIGGER`. |
| Duplicate triggers | Two triggers on the same events calling the same function. |
| Row level security half set up | Row level security on without a policy, or policies with it off. |
| SECURITY DEFINER without a search_path | Routines running with their owner's rights that look names up in the caller's `search_path`. Suggests `SET search_path`. |
| Sequences of another type than their column | A serial or identity column whose sequence is narrower or wider than the column. |
| Sequences nothing uses | No column, view or routine in the schemas mentions them, and no column owns them. |
| Views and routines defined twice | Two views with the same query, or two routines with the same body. |
| Names differing only in case | `Orders` beside `orders`, as tables or as columns of one table. |
| Names that need quoting | Upper case letters, spaces or reserved words. |
| Column types to reconsider | `char(n)`, `money`, `timestamp` without time zone, `timetz` and `serial`, after the wiki's [Don't Do This](https://wiki.postgresql.org/wiki/Don%27t_Do_This). |
| Objects without a comment | Tables, views and routines nobody has described. |

## In the program

```{index} Database Quality Report, context menu; database
```

Right-click a database of a connected server in the browser and choose
**Database Quality Report…**. The wizard is the one the schema documentation
has: the schemas to look at, the format and folder, then a title and an
introduction, and:

Server and database health
: Read the statistics and the server's state too. Unticked, the report
  says only what the catalog does.

Notices
: Include findings that are only worth knowing. Unticked, a big schema's
  report keeps to the warnings and the critical ones.

## On the command line

```{index} slonisko-quality-report; command line
```

```sh
slonisko-quality-report <format> [options] [connection]
```

The format and the connection are taken as by `slonisko-docgen`; the
options are its common ones, and a few of the report's own.

```sh
# The HTML report of every schema, into ./shop-quality-report
slonisko-quality-report html "host=db.example.com dbname=shop user=monitor"

# Every night: Markdown, without notices, failing the job on anything critical
slonisko-quality-report markdown --no-notices --fail-on critical -o reports/shop \
    postgresql://monitor@db.example.com/shop
```

| Option | |
|---|---|
| `-o`, `--output` *dir* | Where to write; by default `<database>-quality-report` in the current directory |
| `-n`, `--schema` *pattern* | Only schemas matching; `*` and `?` are wildcards. May be repeated |
| `-N`, `--exclude-schema` *pattern* | Leave out schemas matching. May be repeated |
| `-t`, `--title` *text* | Title of the report; by default "*database* quality report" |
| `--description` *text* | Introduction for the summary; `@file` reads it from a file |
| `-W`, `--password` | Ask for the password before connecting |
| `--no-health` | Read only the catalog, not the statistics and the server's state |
| `--no-notices` | Leave out the notices |
| `--fail-on` *severity* | Exit with 3 when there is a finding this severe or more: `critical`, `warning` or `notice` |
| `-q`, `--quiet` | Print only errors and warnings |
| `--css` *file* | HTML only: a stylesheet instead of the built-in one |
| `--antora-name` *name*, `--antora-version` *version* | Antora only: the component's name, `report` by default, and version |

It exits with 0 when the report is written, 1 when it cannot be, 2 for a
wrong format or option, and 3 for a finding at or above `--fail-on`. A part
of the server's state that the account may not read is a warning on the
standard error and a notice in the report, not a failure.

## The account it connects as

Any account can read the catalog. For the rest, the account needs to be a
superuser or a member of `pg_monitor` to see other users' sessions and
everything the statistics have:

```sql
CREATE ROLE monitor LOGIN;
GRANT pg_monitor TO monitor;
```

Nothing in the report changes the database: it only reads.
