# Import and backups

## Importing CSV

```{index} import, CSV; import, COPY
```

Right-click a table in the object browser and choose *Import CSV…* to load
a CSV file into it. Right-click a schema and choose *Import CSV into a New
Table…* to make a new table there, shaped after the file.

When a file is chosen, slonisko reads its first lines. From those it works
out the delimiter, fills a preview, and lists the file's columns.

File format
: - **Delimiter**: comma, semicolon, tab or pipe.
  - **Quote**: the quote character.
  - **Encoding**: the file's text encoding.
  - **Skip lines**: lines to leave out at the top, such as a title.
  - **First line names the columns**: whether the first line is a header.
  - **NULLs as**: the text that means NULL. The rules match PostgreSQL's
    own `COPY ... CSV`. When *NULLs as* is left empty, an empty unquoted
    field is NULL and a quoted empty field `""` is an empty string.

Columns
: - **Into an existing table**: each file column goes into the table column
    of the same name. Case does not matter, and spaces match underscores,
    so *First Name* goes into `first_name`. Without a header row, the
    columns are matched in order. Pick another table column, or none, from
    the list in the *Table column* column.
  - **Into a new table**: the columns are named after the header. Each one
    gets a type that fits every value in the preview: `integer`, `bigint`,
    `numeric`, `boolean`, `date`, `timestamp`, `timestamptz` or `text`.
    Names and types can be edited before importing.
  - Unticked columns are not imported. Table columns no file column goes
    into get their defaults.

*Empty the table first* truncates the table before the rows go in.

The import runs on a connection of its own, inside one transaction. Rows go
to the server with `COPY`, a few thousand at a time, so a file of any size
needs little memory, and the progress bar follows the file.

If a row is rejected, the whole import is rolled back, a new table
included, and the dialog names the row. The row number counts data rows
from the first one after the header. *Stop* rolls the import back the same
way.

## Backing up with pg_dump

```{index} backup, pg_dump, dump
```

Right-click a connection or a database and choose *Back Up…*. On a schema
or a table the entry is *Back Up Schema…* or *Back Up Table…*, and the
backup covers only that schema or table.

A wizard runs PostgreSQL's `pg_dump`, a page of choices at a time:

Backup File
: - **Format**:
    - **Custom archive** (the default): one compressed file. `pg_restore`
      reads it and can restore all of it or only some parts.
    - **SQL script**: plain SQL that psql or the SQL editor can run.
    - **Directory**: one file per table, dumped in parallel with *Parallel
      jobs*. Choose a directory that does not exist yet, or an empty one.
    - **Tar archive**.
  - **Save to**: the file, or the directory for a directory backup.
  - **Program**: the `pg_dump` used; see [](#choosing-the-program).

  *Next* stays disabled until the backup can be made, and the page says
  why.

Contents
: - **Contents**: schema and data, the schema only, or the data only.
  - **Schemas** and **Tables**: names separated by commas, to back up only
    those. Tables are written as `schema.table`. Names are taken as they
    are, so case and characters like `*` match literally.

Options
: - **Leave out owners**: no `OWNER TO` statements.
  - **Leave out privileges**: no `GRANT` or `REVOKE` statements.
  - **Data as INSERT statements**: data as `INSERT` statements rather than
    `COPY`.
  - **Compression**.
  - **Drop objects before creating them** and **Create the database**:
    for an SQL script only. An archive leaves both to the restore.

Back Up
: Shows the command to be run. *Back Up* runs it, and what `pg_dump` writes
  appears below the command as it works. *Stop* ends it. To run it again
  with other choices, go *Back*.

## Restoring

```{index} restore, pg_restore, psql; restoring a script
```

Right-click a connection or a database and choose *Restore…*. Choose a
backup on the wizard's first page: a file with *File…*, or a directory
backup with *Directory…*. The page tells which kind of backup it is from
the backup's first bytes:

- **An archive**, in custom, directory or tar format, is restored with
  `pg_restore`.
  - *Contents*, *Schemas* and *Tables* pick which parts to restore.
  - *Drop objects before creating them* drops what the backup holds before
    recreating it.
  - *Create the database* creates the database named in the backup. The
    database chosen is only used to connect from.
- **An SQL script** is run with `psql`. A script does what it says, so
  only two options apply: *All or nothing, in one transaction* and *Stop
  at the first error*. The wizard skips the *Contents* page.

*Stop at the first error* is on by default. Without it, the restore carries
on past errors and reports them at the end. *All or nothing, in one
transaction* rolls back the whole restore if anything fails. It cannot be
combined with parallel jobs.

The last page shows the command; *Restore* runs it. slonisko asks before
restoring, naming the database and the connection.
When the restore is done, the browser and completion read the database
again.

## Choosing the program

```{index} pg_dump; version
```

`pg_dump` refuses to back up a server newer than itself. Of the copies
slonisko finds, it uses the first that is at least as new as the server.
It looks on `PATH` first, then where PostgreSQL's packages install each
version side by side:

- Debian and Ubuntu: `/usr/lib/postgresql/18/bin`
- openSUSE: `/usr/lib/postgresql18/bin`
- Red Hat and Fedora: `/usr/pgsql-18/bin`
- macOS: Postgres.app and Homebrew
- Windows: `C:\Program Files\PostgreSQL\18\bin`

The wizard's first page shows which copy it uses and its version. If that
copy is older than the server, the page warns. *Choose…* picks another
copy. See [Client programs](configuration.md#client-programs).

## Passwords

`pg_dump`, `pg_restore` and `psql` connect the same way slonisko does,
through the SSH tunnel when the connection uses one. The password is
handed to them in their environment, not on the command line, so other
users of the machine cannot see it in the process list. The command shown
in the dialog has no password for the same reason.
