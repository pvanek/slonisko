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

## Connection profiles

```{index} connection profiles
```

A profile holds what libpq needs — host, port, database, user, SSL mode and
certificates, application name, connect timeout and any further parameters
— plus what the program needs: a display name, a colour, whether to
browse every database of the server, and the SSH tunnel settings.

Profiles can be duplicated, which is the quickest way to make the test and
production entries of the same server differ only by host and colour.

## Row limit

```{index} row limit
```

Queries in a SQL editor fetch rows in chunks of a thousand and stop at the
row limit of 100,000 rows, so a careless `SELECT *` on a huge table does
not fill memory. The limit cannot be changed yet. What was fetched is what
the grid holds; exporting *all rows* runs the query again and streams
everything to the file.
