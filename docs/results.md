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

## Export

```{index} export, CSV, SQL INSERT, clipboard
```

The *Export* button copies or saves the rows. Its *Export…* entry opens a
dialog that asks three things:

Which rows
: The rows that were fetched, the rows selected in the grid, or **all
  rows** — and if the query was stopped at the row limit, "all rows" runs it
  again and writes the rows to the file as they arrive, so a result far
  bigger than memory still goes through.

In which format
: CSV, tab-separated, the text table, `INSERT` statements, or bulk
  `INSERT`s with several rows per statement. Each format brings its own
  options: a header row, a delimiter, what NULL is written as, the table
  name for SQL, how many rows go in one bulk `INSERT`.

Where to
: A file, or the clipboard — except for rows that are still on the
  server, which only go to a file.

```{figure} images/export-dialog.png
:alt: The export dialog

The export dialog, set up for CSV with a header row.
```

The rest of the menu has the everyday shortcuts: *Copy as CSV*, *as Text
table*, *as SQL INSERT*, which take the selected rows when there are any.

## Plan

```{index} EXPLAIN, query plan
```

Explaining a statement opens the *Plan* tab instead of rows: the plan as a
tree, with cost, estimated and actual rows, loops, time and each node's own
share of the total, or the same plan as the text `psql` would print. The
summary line above says whether the plan was executed and what it cost.

```{figure} images/plan.png
:alt: An executed plan as a tree, the nodes with the largest own share marked in red

A plan from {kbd}`Ctrl+Shift+E`: executed, rolled back, and the expensive nodes marked.
```
