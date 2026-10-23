# Results

```{index} results, result views
```

Every page that runs a query shows its rows the same way: a tab bar down the
left side switches between three views of the same result, and the footer
says how many rows came back and how long it took.

```{figure} images/results-editing.png
:alt: The grid while editing: one changed cell highlighted, the editing tools above the rows

The grid with the editing tools open and two changed cells waiting to be saved.
```

## Grid

```{index} grid, editing rows, primary key, sorting
```

The default: one row per row, columns sized to their contents and their
headers, numbers right-aligned, NULLs shown as `NULL` in grey. A long value
is cut short in the cell; its tooltip shows more of it.

On DBA Tools and System Info pages, **clicking a column's header sorts by
it**; a second click sorts the other way, a third goes back to the order
the query gave. Numbers sort by value, sizes such as `8192 B` or
`1.5 GB` by size, text naturally (`B2` before `b10`), and NULLs come last
either way. The order is kept when the page runs again, and the text view,
the record view, copying and exporting all follow it. A SQL editor's grid
keeps the order of the query, as its rows can be edited.

In a SQL editor, rows can be **edited in place** when the result comes
from a single table, that table has a primary key and the result includes
its columns — the footer says *Editing schema.table*, or *Read-only* with
the reason in its tooltip. The editing tools live in a palette that opens
with the *Edit* button: add a row, delete rows, set a cell to NULL, save or
discard. Changed cells are highlighted and deleted rows struck through
until they are saved; deleting a row again takes the deletion back. Saving
shows the `INSERT`/`UPDATE`/`DELETE` statements first, runs them in one
transaction (or a savepoint, inside an open one), and keeps none of them if
any statement would change more than its own row.

## Text

```{index} text view, block selection
```

The same rows as an ASCII table, in a monospace font, ready to paste into a
message or a file. **{kbd}`Alt`+drag selects a block**, so a single column
can be copied out of the table. It shows the first 10,000 rows at most;
an export writes them all.

```{figure} images/results-text.png
:alt: The same rows as an ASCII table

The text view.
```

On a desktop where the window manager takes {kbd}`Alt`+drag for moving
windows, use {kbd}`Alt+Shift` with the arrow keys instead, or change the
window manager's modifier.

## Record

```{index} record view
```

One row at a time, its columns under each other — useful for a wide table.
The arrows below the form step through the rows, and the view follows the
row selected in the grid.

```{figure} images/results-record.png
:alt: One row with its columns under each other

The record view, on the fifth row.
```

## Viewing a value

```{index} value viewer, JSON, jsonb, arrays, long text
```

A cell shows the first few hundred characters of its value on one line. To
read all of it, select the cell and press {kbd}`Shift+Enter`, or choose
*View Value…* from the right-click menu. Double-clicking a cell does the
same in the record view, and in a grid whose rows cannot be edited; in an
editable grid, double-clicking changes the value instead. The value opens
in a window of its own, read-only, and several can be open at once. Each
window keeps the value it opened with, even when the query runs again.

*Show as* says how to read the value: `json` and `jsonb` columns are shown
as JSON and array columns as arrays. In a text column, a value that is a
whole JSON object or array is shown as JSON too. Otherwise, and for any
value you choose to read differently, pick it from the list.

*Format*
: Puts each JSON member or array element on a line of its own, indented by
  four spaces. Members stay in their order and numbers stay as they were
  written, which a round trip through a JavaScript-style parser would not
  keep. In an array of arrays, each innermost array — a row of a matrix —
  stays on one line. A value that does not read as what was chosen is shown
  as it is, and the line at the bottom says so.

Highlighting
: Keys, strings, numbers, `true`, `false` and `null` (`NULL` in arrays) get
  the editor's colours; anything out of place is red, so a stray comma
  stands out.

*Wrap lines*
: Wraps long lines at the window's edge, for a long text rather than a
  document.

*Copy* puts the value on the clipboard as it is shown, formatted or not;
{kbd}`Ctrl+C` copies the selected part. *Format* and *Wrap lines* are kept
for the next value. A NULL has nothing to show, so *View Value…* is greyed
out for it.

## Export

```{index} export, CSV, JSON, Markdown, HTML, XML, SQL INSERT, SQL COPY, clipboard
```

The *Export* button copies or saves the rows. Its *Export…* entry opens a
dialog that asks three things:

Which rows
: The rows that were fetched, the rows selected in the grid, or **all
  rows** — and if the query was stopped at the row limit, "all rows" runs it
  again and writes the rows to the file as they arrive, so a result far
  bigger than memory still goes through.

In which format
: - CSV or tab-separated values.
  - JSON, as an array with an object per row, or JSON Lines, an object
    per line. Numbers and booleans are written as JSON numbers and booleans,
    and `json` and `jsonb` values are embedded as they are, not as strings.
  - A Markdown table.
  - An HTML page holding a table, which a browser or a spreadsheet opens.
  - XML.
  - The text table.
  - SQL `INSERT` statements, or bulk `INSERT`s with several rows per
    statement.
  - SQL `COPY`: a `COPY ... FROM stdin` statement followed by the rows, as
    pg_dump writes them. Both psql and the SQL editor run it as it is.

  Each format brings its own options: a header row, a delimiter, what NULL
  is written as, the table name for SQL, how many rows go in one bulk
  `INSERT`.

Where to
: A file, or the clipboard — except for rows that are still on the
  server, which only go to a file.

```{figure} images/export-dialog.png
:alt: The export dialog

The export dialog, set up for CSV with a header row.
```

The rest of the menu has the everyday shortcuts: *Copy as CSV*, *as JSON*,
*as Markdown*, *as Text table* and *as SQL INSERT*. They copy the selected
rows, or every row when nothing is selected.

To load rows into a table from a CSV file, see
[Importing CSV](import-and-backup.md#importing-csv).

## DBA Tools and System Info

```{index} DBA tools, sessions, cancelling a query, terminating a session, auto-refresh
```

Opening an item under *DBA Tools* or *System Info* in the connection tree
shows its rows on a page of their own, which runs its query on a connection
of its own.

**Auto-refresh**, the list in the page's header, runs the query again every
2, 5, 10, 30 or 60 seconds, but only while the page is shown: a page behind
another tab waits, and catches up when you come back to it. Refreshing keeps
the column widths, the sort order and where the rows were scrolled to.
The interval is not remembered after the page closes.

The pages that list server processes — *Sessions*, *Locks*, *Blocked
Sessions*, *Long-Running Queries* and *Vacuum Progress* — can also stop
them. Select their rows, any cell will do, and use the header's buttons or
the right-click menu:

*Cancel Query*
: Stops what the session is running, as {kbd}`Ctrl+C` would in psql. The
  session stays connected; a transaction it was in is left failed, to be
  rolled back.

*Terminate Session*
: Ends the session. Its transaction is rolled back and its client
  disconnected.

Both ask first, naming the process IDs. The line under the rows says how it
went: a process may have ended already, or belong to a role that yours is
not allowed to signal (that takes superuser, or membership in the role or
in `pg_signal_backend`). The selected sessions stay selected as the page
refreshes, until they are gone.

## Pinned results

```{index} pinned results, pinned plans, result tabs
```

In a SQL editor, each statement's rows replace the last ones under
*Results*. To keep some, click *Pin* above them, or right-click the
*Results* tab and choose *Pin*. They move to a tab of their own, named after
their query, and the next statement's rows go under *Results* as usual.

To give a pinned tab a name of your own, right-click it and choose
*Rename…*, type over the name and press {kbd}`Enter`; {kbd}`Esc` keeps the old
one, and an empty name brings back the statement. The tab's tooltip shows
the statement whatever its name.

A pinned tab stays until you close it with its close button, or with *Close
All Pinned* from the tab's context menu. Its rows can still be edited and
saved, exported, and run again with *Run again*, which refreshes them in
the same tab. Closing a tab while its query is still running stops the query.

Plans pin the same way: *Pin* beside the plan's summary, or on the *Plan*
tab's context menu, keeps it in a tab named *Plan:* and its statement, so
that the next EXPLAIN can be compared with it. Pinned rows come before the
*Plan* tab and pinned plans after it.

## Plan

```{index} EXPLAIN, query plan
```

Explaining a statement opens the *Plan* tab instead of rows. The summary
line above says whether the plan was executed and what it cost. The tabs
down the left side show the same plan four ways:

*Tree*
: Each node with its cost, estimated and actual rows, loops, time and its
  own share of the total.

*Text*
: The plan as `psql` would print it.

*Diagram*
: A box per node, the first to run at the bottom and the root at the top.
  The lines carry the rows each node hands to the one above it: the more
  rows, the thicker the line. A box is tinted by the node's own share of
  the time, or of the cost when the plan was not executed, and a bar along
  its bottom shows that share. A row count off from its estimate by more
  than ten times is shown in orange. Drag to move around, and zoom with
  {kbd}`Ctrl` and the mouse wheel. A plan too big to fit opens shrunk, but
  only as far as it stays readable.

*Flame Graph*
: Where the time went. Each node is a bar as wide as the time spent in it
  and in the nodes it runs, which sit on top of it. The root is at the
  bottom and spans the whole width. Without ANALYZE the widths are costs.
  The colour goes from yellow to red with the node's own share. Click a bar
  to widen it to the whole width; click one below it to go back.

A node chosen in one view is chosen in the others. Double-click a box or a
bar to see the node in the tree.

*Export* beside the summary copies or saves the diagram and the flame
graph:

- **Pictures:** SVG, PNG or PDF, drawn black on white whatever the
  program's colours. The diagram comes out whole. The flame graph comes
  out zoomed as it is shown, and only as tall as its bars.
- **Diagram as text:** Graphviz (`.dot`) or Mermaid (`.mmd`) source,
  copied to the clipboard or saved. Those tools lay the plan out
  themselves.
- **Flame graph as text:** *folded stacks*, the format `flamegraph.pl`,
  [speedscope](https://www.speedscope.app/) and similar tools read. Each
  line lists the operations from the root down to one node, then what that
  node took itself: whole microseconds when the plan was executed,
  hundredths of a cost unit when it was not.

```{figure} images/plan.png
:alt: An executed plan as a tree, the nodes with the largest own share marked in red

A plan from {kbd}`Ctrl+Shift+E`: executed, rolled back, and the expensive nodes marked.
```
