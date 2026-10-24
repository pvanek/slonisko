# The SQL editor

```{index} SQL editor, editor
```

The editor is QScintilla with PostgreSQL's own parser behind it: the
highlighting, the completion and the error marks all come from the same
grammar the server uses.

Names the catalog knows are coloured as what they are — tables, columns,
functions — and resting the pointer on one says more: a column's type, the
table it belongs to. A table that neither the catalog nor the script knows,
or an `alias.column` the table does not have, gets an amber wavy
underline. Resting on a red error mark shows the server's message.

## Running

```{index} row limit, cancelling a statement
```

{kbd}`Ctrl+Enter`
: Runs the statement under the cursor, or everything selected. The piece
  about to run is outlined while the cursor sits in it.

{kbd}`Ctrl+E` / {kbd}`Ctrl+Shift+E`
: Explains the statement, or explains it with `ANALYZE` — the second one
  runs the statement inside a transaction that is rolled back afterwards.

{kbd}`Ctrl+.`
: Stops what is running. The server is asked to cancel; the editor says so
  in *Messages*.

A script may hold any number of statements; they run one after another and
stop at the first error, which is marked where it happened. Statements are
split the way psql splits them, so `$$ … $$` bodies, `BEGIN ATOMIC` blocks
and semicolons inside quotes are no trouble.

A query stops fetching after 100,000 rows; the grid then says it shows
only the first ones.

```{figure} images/error.png
:alt: A misspelt column underlined in the script, with the server's error and hint in Messages

An error: the misspelt column is underlined in the script, the server's message and hint are in *Messages*.
```

## Find and replace

```{index} find, replace, search, regular expressions
```

Searching works as in Kate: a bar opens below the editor rather than a
dialog over it, and the text stays in view while you search.

{kbd}`Ctrl+F`
: Opens the bar to find as you type. Each letter moves the selection to the
  next match after where the cursor was, and every match in the script is
  highlighted. The field turns green when there is a match and red when
  there is none. {kbd}`Enter` and {kbd}`Shift+Enter` go to the next and the
  previous match, and searching starts over at the other end of the script
  when it runs out, saying so. Typing searches for plain text.

{kbd}`Ctrl+R`
: Opens the bar with replace and the search options. Here searching waits
  for {kbd}`Enter`. *Replace* (or {kbd}`Enter` in the replace field)
  replaces the selected match and selects the next one; if the selection is
  not a match yet, it finds one first, so you see each one before it
  changes. *Replace All* replaces every match at once, and a single
  {kbd}`Ctrl+Z` takes all of it back. *Find All* highlights and counts the
  matches.

{kbd}`F3` / {kbd}`Shift+F3`
: Find the next or previous match again, with the bar open or closed.

{kbd}`Ctrl+H` / {kbd}`Ctrl+Shift+H`
: Find the selected text, or the word at the cursor, forwards or backwards.

{kbd}`Esc`
: Closes the bar, in the bar or in the editor, and takes the highlighting
  away. The last match stays selected. The key that opened the bar,
  {kbd}`Ctrl+F` or {kbd}`Ctrl+R`, closes it too while you are in the bar;
  pressed in the editor, it takes you back to the bar.

Text selected on one line is what the bar searches for when it opens.
Selecting several lines and pressing {kbd}`Ctrl+R` checks *Selection only*:
the search and *Replace All* then keep to those lines, which stay faintly
marked and move with the text as you edit around them.

The text to find is read one of four ways:

Plain text
: Exactly what is typed.

Whole words
: Plain text that is not part of a longer word, so `id` finds `id` but not
  `user_id`.

Escape sequences
: Plain text where `\n` is a line break, `\t` a tab and `\\` a backslash.

Regular expression
: Perl-compatible (PCRE2), which is close to but not quite PostgreSQL's `~`. `^`
  and `$` match at the start and end of every line, and `\R` matches any
  line break. In the replacement, `\0` is the whole match and `\1` to `\9`
  are its groups. `\U` and `\L` switch what follows to upper or lower case
  until `\E`, and `\u` and `\l` change only the next letter. Replacing `^`
  with `-- ` comments every line out.

In escape-sequence and regular expression modes, `\n` in the replacement is
a line break written the way the file writes them.

The last fifteen searches and replacements are kept in the fields' lists
from one session to the next, along with the search mode and *Match case*.

## Completion

```{index} completion, fuzzy matching
```

Completion offers what makes sense where the cursor is: tables after `FROM`,
columns of the tables in scope inside the select list and `WHERE`, functions,
types, schemas. It opens by itself after three characters or a dot, or on
{kbd}`Ctrl+Space`; {kbd}`Enter` or {kbd}`Tab` takes the choice.

```{figure} images/completion.png
:alt: The completion list after an alias, offering the columns of the customer table with their types

After `c.` the list offers the columns of the table `c` stands for, each with its type.
```

Matching is not only by prefix. Typing `leapa` finds `learning_package`,
`cnm` finds `customer_name`, and the letters that matched are highlighted in
the list, so it is clear why something is being offered.

### Aliases

```{index} aliases, table aliases
```

A table, view, materialized view or foreign table chosen after `FROM` or
`JOIN` comes with an alias made of the initials of its name: `foo_bar` goes
in as `foo_bar fb`, `OrderItems` as `"OrderItems" oi`. The alias is one the
statement does not use yet, so the same table joined a second time gets
`fb2`, a third time `fb3`; an initial that is a keyword, like `do` for
`data_object`, gets a number too. The same goes for `UPDATE … FROM`,
`DELETE … USING` and `MERGE … USING`.

No alias is added where it is not wanted: for the table an `INSERT`,
`UPDATE`, `DELETE` or `TRUNCATE` works on, or when the name being completed
has an alias after it already.

### Snippets

```{index} snippets, abbreviations
```

Short words stand for whole statements. Type one and press {kbd}`Enter`;
the two-letter ones need {kbd}`Ctrl+Space` first, as the list does not open
by itself for them:

| Type | You get |
|---|---|
| `sf` | `SELECT * FROM` |
| `scf` | `SELECT count(1) FROM` |
| `sfw` | `SELECT * FROM … WHERE` |
| `sfl` | `SELECT * FROM … LIMIT 100` |
| `ins` | `INSERT INTO … () VALUES ()` |
| `upd` | `UPDATE … SET … WHERE` |
| `del` | `DELETE FROM … WHERE` |
| `ct`, `ctas` | `CREATE TABLE …`, `CREATE TABLE … AS SELECT` |
| `cte` | a `WITH` query with its `SELECT` |
| `ea` | `EXPLAIN (ANALYZE, BUFFERS)` |
| `tx` | `BEGIN;` … `COMMIT;` |

The caret lands where the rest is to be typed — mostly a table's name — and the keywords follow the
case of the abbreviation: `sf` writes lowercase, `SF` writes capitals.

## Opening what a name is

```{index} Ctrl+click, object details
```

Hold {kbd}`Ctrl` and the pointer turns into a hand over the names of
tables, views and other relations the catalog knows. {kbd}`Ctrl`+click
opens that object's details — its columns, indexes, constraints and
diagram — in a page of its own. Columns, functions, names that resolve to
nothing, aliases and CTEs are not links.

```{figure} images/object-page.png
:alt: The page of a table, on its Columns tab

A table's page: an overview, then its columns, indexes, constraints, triggers and diagram.
```

## Notifications

```{index} LISTEN, NOTIFY, notifications
```

After [`LISTEN`](https://www.postgresql.org/docs/current/sql-listen.html) in an editor, every
[`NOTIFY`](https://www.postgresql.org/docs/current/sql-notify.html) on that channel shows in the page's
*Messages*, with the sending server process and the payload — also while
the editor sits idle. The editor's connection is its own, so another
editor does not hear them; `UNLISTEN` stops them.

## psql commands and variables

```{index} psql, psql variables, COPY
```

Scripts written for `psql` mostly run as they are. Meta-commands such as
`\d` are skipped with a note rather than sent to the server, `\set`,
`\unset` and `\echo` work, and `:name`, `:'name'` and `:"name"` are
substituted before a statement runs —
with error positions still pointing at the right place in the script.

`COPY … FROM stdin` followed by its data block is sent as one piece, so a
dump file can be replayed from the editor.

## Files

```{index} opening files, saving files
```

{kbd}`Ctrl+O` opens a file, {kbd}`Ctrl+S` saves, {kbd}`Ctrl+Shift+S` saves
under another name. A page with unsaved changes says so in its tab and asks
before it closes. The file browser on the left opens files by double-click;
the box above its list filters them by name, `*.sql` to begin with.
