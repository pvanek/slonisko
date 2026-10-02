# Getting started

## The window

```{index} workspace, pages, object browser
```

slonisko opens with three areas:

```{figure} images/main-window.png
:alt: The main window: the object browser on the left, a SQL editor with its results on the right

The object browser on the left, a SQL editor and its results on the right.
```

- **Left** — connections and files, on tabs down the side. The connection
  tab is the object browser: servers, their databases, schemas and
  everything in them, plus DBA tools and system information.
- **Right** — the workspace, where every tab is a page: a SQL script with
  its results, a monitoring query, or the details of one object.
- **Bottom of each page** — what the page has to say: row counts, timings,
  messages from the server.

Any page can be pulled out into a window of its own: right-click its tab and
choose *Move to New Window*. The window is independent — it can go behind
the main one, or to another screen — and *Move to Main Window* on its tab
puts it back.

## The first connection

```{index} new connection, connection colour, SSH tunnel
```

Press the *New Connection* button above the connection tree, or use the
tree's context menu. The dialog asks for the usual libpq settings: host,
port, database, user, and how the password is handled. Two things worth
setting while you are there:

```{figure} images/connection-dialog.png
:alt: The connection dialog, on its General tab

The connection dialog. SSL, the SSH tunnel and further libpq settings have tabs of their own.
```

Name and colour
: The name is what the tree, the page tabs and the editor's connection box
  show. The colour marks the connection in the tree and on its pages' tabs,
  and tints the editor's connection box, which is what stops a statement
  meant for a test server from running against production.

SSH tunnel
: slonisko runs the system `ssh` client for you and connects through the
  tunnel, signing in with the SSH agent, a key file or a password. The
  password is asked for when connecting, or taken from the password store;
  a key's passphrase, if it has one, has to be saved there.

See {doc}`connections` for what happens after that: how connections and
transactions relate, and where passwords are kept.

## Running a statement

```{index} running statements, Explain, Explain Analyze
```

Type SQL in the editor and press {kbd}`Ctrl+Enter`. Without a selection,
the statement under the cursor is run; with one, everything selected. The
statement about to run is outlined faintly, so there is never a doubt about
what {kbd}`Ctrl+Enter` will do.

Rows appear in the grid below the editor, errors in *Messages* with the
offending word underlined in the script.

{kbd}`Ctrl+E` explains the statement without running it; {kbd}`Ctrl+Shift+E`
runs it with `EXPLAIN (ANALYZE, BUFFERS)` inside a transaction that is
rolled back afterwards, so nothing is written.

## The manual, from inside the program

```{index} help, manual, F1
```

Help → Manual opens this manual in a window of its own, with its contents,
an index and a search box. {kbd}`F1` opens it on the page that matches what
you are looking at: an editor page opens *The SQL editor*, a result page
*Results*, an object page *Diagrams*.

If the program was installed without its help file, the same menu item
opens the project's page on GitHub instead.

## Where to go next

- {doc}`editor` — completion, snippets, psql commands, running files.
- {doc}`results` — the grid, text and record views, editing rows, export.
- {doc}`diagrams` — the ER diagrams of a table, a schema or the database.
- {doc}`shortcuts` — the keys worth learning first.
