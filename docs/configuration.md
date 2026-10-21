# Configuration

```{index} configuration, settings file
```

## Where things are kept

Connection profiles, window geometry, and the last directories of the file
browser and of the open and save dialogs live in the usual place for the
platform, through `QSettings`:

- Linux — `~/.config/yarpen.cz/slonisko.conf`
- macOS — `~/Library/Preferences/cz.yarpen.slonisko.plist`
- Windows — the registry, under `HKEY_CURRENT_USER\Software\yarpen.cz\slonisko`

Passwords are not in there unless they have to be: see
[Passwords](connections.md#passwords).

## Client programs

```{index} pg_dump; location, pg_restore; location, psql; location
```

Backups and restores run PostgreSQL's own `pg_dump`, `pg_restore` and
`psql`. slonisko looks for them on `PATH` and in the directories where
PostgreSQL's packages install each version. *Choose…* in the backup and
restore wizards picks another copy; its directory is remembered as
`clientTools/directory`, and slonisko then uses the programs from that
directory whatever their version. See
[Choosing the program](import-and-backup.md#choosing-the-program).

## Connection profiles

```{index} connection profiles
```

A profile holds what libpq needs — host, port, database, user, SSL mode and
certificates, application name, connect timeout and any further parameters
— plus what the program needs: a display name, a colour, whether to
browse every database of the server, and the SSH tunnel settings.

Profiles can be duplicated, which is the quickest way to make the test and
production entries of the same server differ only by host and colour.

## Connection folders

```{index} connection folders, folders
```

Profiles can be sorted into folders, nested as deep as you like — by
environment, by customer, by whatever keeps a long list manageable:

```
dev
  conn1
stage
  env1
    conn2
    conn3
  foo
    conn4
  conn5
```

Folders are managed in the connection tree. *New Folder*, in its toolbar
or context menu, makes one inside the folder that is selected, or the
folder of the selected connection.

Connections and folders stay in the order you put them in. Drag one onto a
folder to move it to the end of that folder, or between two others to put
it there; the empty space below the tree is the end of the top level.
*Move to Folder* in their context menu moves them without the mouse. New
connections and folders start at the end of their folder. A folder moved
where one of the same name exists merges into it.

*Rename Folder* (F2) renames one, and *Delete Folder* removes it without
deleting anything in it: its connections and folders move up a level.
Empty folders are kept, and so is which folders were left open.

## Row limit

```{index} row limit
```

Queries in a SQL editor fetch rows in chunks of a thousand and stop at the
row limit of 100,000 rows, so a careless `SELECT *` on a huge table does
not fill memory. The limit cannot be changed yet. What was fetched is what
the grid holds; exporting *all rows* runs the query again and streams
everything to the file.
