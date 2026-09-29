# Results

```{index} results, result views
```

Every page that runs a query shows its rows the same way: a tab bar down the
left side switches between three views of the same result, and the footer
says how many rows came back and how long it took.

## Grid

```{index} grid, editing rows, primary key
```

The default: one row per row, sortable columns sized to their contents and
their headers, numbers right-aligned, NULLs shown as `NULL` in grey.

Rows can be **edited in place** when the result comes from a single table
and that table has a primary key — the footer says *Editing schema.table*,
or why not. The editing tools live in a palette that opens with the *Edit*
button: add a row, delete rows, set a cell to NULL, save or discard. Saving
shows the `INSERT`/`UPDATE`/`DELETE` statements first, runs them in one
transaction (or a savepoint, inside an open one), and keeps none of them if
any statement would change more than its own row.

## Text

```{index} text view, block selection
```

The same rows as an ASCII table, in a monospace font, ready to paste into a
message or a file. **{kbd}`Alt`+drag selects a block**, so a single column
can be copied out of the table.

On a desktop where the window manager takes {kbd}`Alt`+drag for moving
windows, use {kbd}`Alt+Shift` with the arrow keys instead, or change the
window manager's modifier.

## Record

```{index} record view
```

One row at a time, its columns under each other — useful for a wide table.
The arrows below the form step through the rows, and the view follows the
row selected in the grid.

## Export

```{index} export, CSV, SQL INSERT, clipboard
```

The *Export* button copies or saves the rows. The dialog asks three things:

Which rows
: The rows that were fetched, the rows selected in the grid, or **all
  rows** — and if the query was stopped at the row limit, "all rows" runs it
  again and writes the rows to the file as they arrive, so a result far
  bigger than memory still goes through.

In which format
: CSV, tab-separated, the text table, `INSERT` statements, or bulk
  `INSERT`s with several rows per statement. Each format brings its own
  options: a delimiter, what NULL is written as, the table name for SQL.

Where to
: A file, or the clipboard.

The menu beside it has the everyday shortcuts: *Copy as CSV*, *as text*, *as
SQL INSERT*, which take the selected rows when there are any.

## Plan

```{index} EXPLAIN, query plan
```

Explaining a statement opens the *Plan* tab instead of rows: the plan as a
tree, with cost, estimated and actual rows, loops, time and each node's own
share of the total, or the same plan as the text `psql` would print. The
summary line above says whether the plan was executed and what it cost.
