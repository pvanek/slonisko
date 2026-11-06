# Comparing schemas

```{index} compare schemas, schema diff, migration script, alignment script, pg-schema-diff
```

**Tools → Compare Schemas…** compares the schemas of two databases and
writes the SQL that makes one like the other: production against the
development database it should catch up with, a staging copy against the
one it was copied from, two databases of the same server. The idea is that of Stripe's
[pg-schema-diff](https://github.com/stripe/pg-schema-diff): you say what
the schema should be, and the script says how to get there.

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
