# Comparing schemas

```{index} compare schemas, schema diff, migration script, alignment script, pg-schema-diff, slonisko-schemadiff
```

Slonisko compares the schemas of two databases and writes the SQL that
makes one like the other: production against the
development database it should catch up with, a staging copy against the
one it was copied from, two databases of the same server. The idea is that of Stripe's
[pg-schema-diff](https://github.com/stripe/pg-schema-diff): you say what
the schema should be, and the script says how to get there. It comes in two forms that write the
same script:

- **Tools → Compare Schemas…** in the program, on two of the browser's
  connections;
- **`slonisko-schemadiff`**, a command line tool installed with it, for
  scripts and continuous integration: to fail a job when a database has
  drifted from the schema it should have, or to write the script for
  someone to review.

The two sides are called **Reference** and **Target**. Reference is the
schema to match, and is only read. The script runs on Target, and once it
has, Target's schemas are those of Reference. So *added* means in Reference
but not yet in Target, and *removed* in Target but no longer in Reference.

## The wizard

```{index} single: compare schemas; wizard
```

Databases
: A saved connection and a database for each side. The wizard starts with
  the ones compared last time, and with nothing chosen the first time. The database list is the server's
  once the connection is open; any name can be typed. Both sides may be the
  same connection, as long as the databases differ. Connections that are
  not open yet are opened by **Next**, as the browser would open them,
  asking for a password if one is not saved.

Schemas
: Every schema of either database, all of them checked. Only the checked
  ones are compared, on both sides: a schema left out is neither created
  nor dropped.

Differences
: What differs, one line each: added, removed or changed, which object,
  what about it differs, and its hazards. Below, the whole script; clicking
  a difference shows its statements in it. **Open in SQL Editor** puts the
  script into a new editor on Target, to read and run there; **Save…**
  writes it to a file, and **Copy** to the clipboard.

## What is compared

```{index} single: compare schemas; objects
```

Schemas, extensions, enums, composite and range types, domains, sequences,
functions and procedures, tables with their columns, keys, checks, indexes,
triggers, row level security and policies, partitions, views, materialized
views and their indexes, and the comments of all of them.

Objects are matched by schema and name; a function by its arguments too,
so a function whose arguments change is one removed and another added.
Their definitions are compared as the server spells them, read with
every name qualified by its schema, so two databases with different search
paths compare the same and the script runs whatever the search path.

Left out:

- owners, privileges and tablespaces, which depend on the server rather
  than on the schema;
- what an extension made: the extension itself is compared;
- foreign tables, and aggregates beyond whether they exist;
- the indexes, keys and triggers of a partition, which come from its
  partitioned table: partitions are compared by their table and bounds;
- the data, and where a sequence has got to.

## The script

```{index} single: compare schemas; script, hazards
```

Drops come first, of what depends on others before what it depends on;
then everything made or changed, the other way round; and last the drops of
types, functions, sequences, extensions and schemas, once nothing uses them
any more. A column can so move from an old type to a new one before the old
one goes.

Some things are done the way that keeps a busy database going:

- a new check or foreign key on an existing table is added `NOT VALID` and
  validated afterwards, which does not stop writes to it;
- with **Build indexes concurrently**, indexes on existing tables are built
  and dropped `CONCURRENTLY`. A script with those cannot run in a
  transaction, so it has none;
- new enum values are added before the transaction begins: a value cannot
  be used in the transaction that added it.

Otherwise the script runs in one transaction, unless **In one transaction**
is unchecked: if any statement fails, nothing has changed.

A view cannot be altered when what it reads changes, so a view is dropped
and made again when its definition changes, when a column it may read is
dropped or changes type, and when a view it reads is made again. So is a
function whose result type changes, which `CREATE OR REPLACE` cannot do.

Each change in the script is headed by a comment saying what it is, and by
its hazards:

data loss
: A table, a column or an extension goes, and its data with it.

table rewrite
: The table is written again, holding a lock that stops reads and writes
  meanwhile: a column changing type, a stored generated column added.

blocks writes
: The table is scanned or an index is built while writes wait: `SET NOT
  NULL`, a new primary key or unique constraint, an index built without
  `CONCURRENTLY`.

needs attention
: Cannot be done in place: an enum losing a value, a domain or range
  changing type, a table partitioned another way. The statements that would
  do it are left in the script commented out, for whoever knows the
  database to decide.

Read the script before running it, and run it on a copy first. Only whoever
knows the data can tell whether a column may go, or whether a `NOT NULL`
column added to a table with rows needs a default.

## On the command line

```{index} slonisko-schemadiff; command line, schema drift
```

```sh
slonisko-schemadiff [options] <target> <reference>
```

Target first, as in the wizard's script: the database the script is for,
then the one whose schema it should have. Both are libpq connection strings
or URIs, as `psql` takes them; for whatever they leave out, libpq's `PG*`
environment variables and `~/.pgpass` apply. The script goes to the
standard output, and a count of the differences to the standard error.

```sh
# The script that brings production up to staging, for review
slonisko-schemadiff -o align.sql \
    "host=prod.example.com dbname=shop user=reader" \
    "host=staging.example.com dbname=shop user=reader"

# In CI: fail when the database built from the migrations differs from the
# expected one, listing what differs
slonisko-schemadiff --check -n app "dbname=migrated" "dbname=expected"
```

| Option | |
|---|---|
| `-o`, `--output` *file* | Where to write the script, or with `--check` the differences; by default the standard output |
| `-n`, `--schema` *pattern* | Only schemas matching; `*` and `?` are wildcards. May be repeated |
| `-N`, `--exclude-schema` *pattern* | Leave out schemas matching. May be repeated |
| `-W`, `--password` | Ask for the password before connecting; it is used for both databases |
| `--concurrently` | Build and drop indexes on existing tables `CONCURRENTLY`, as **Build indexes concurrently** does |
| `--no-transaction` | Leave out `BEGIN` and `COMMIT`, as unchecking **In one transaction** does |
| `--check` | Write the differences instead of the script, and exit with 3 when there are any |
| `--exit-code` | Exit with 3 when the schemas differ; the script is written all the same |
| `-q`, `--quiet` | Print only errors |

`--check` writes one difference a line, as the script heads it: added,
removed or changed, which object, and what about it differs; then, after a
tab, its hazards, if it has any.

```text
removed column shop.orders.note	data loss
changed column shop.orders.total: type integer → bigint	table rewrite
added index orders_state on shop.orders	blocks writes
```

It exits with 0 when the script or the list is written, 1 when it cannot
be (a database it cannot connect to or read, a file it cannot write), 2
for options it does not understand or not two connections, and 3 when the
schemas differ and `--check` or `--exit-code` is given.

It is installed where `slonisko-docgen` is (see
{doc}`its section <docgen>`); in the Flatpak it is run as

```sh
flatpak run --command=slonisko-schemadiff cz.yarpen.slonisko --help
```

Both accounts only need to read the catalog, which any account can.
Nothing is run on either database: the script is only written.

