# Diagrams

```{index} diagrams, ER diagram
```

Tables, schemas and databases have a *Diagram* tab showing their foreign
keys. The diagram is drawn by the program itself — there is no Graphviz or
other outside tool involved — and it is loaded only when the tab is opened.

## What is drawn

```{index} crow's foot notation, foreign keys
```

Only **key columns** are listed in each box: the primary key in bold, the
foreign keys in italics, and a last line saying how many other columns the
table has. A diagram is about how tables relate, not about every column.

Relationships use crow's foot notation. Read each end outwards from the box:

- the **foot** with a circle — zero or many rows on that side;
- **two bars** — exactly one, which is what a `NOT NULL` foreign key means;
- **a bar and a circle** — zero or one, a nullable foreign key.

A key pointing at its own table is drawn as a loop on the side of the box.

## The three scopes

```{index} table diagram, schema diagram, database diagram
```

Table
: The table in the middle, what it references around the upper half, what
  references it around the lower half. Double-clicking a neighbour opens
  that table's own page.

Schema
: Every table of the schema, in layers: a referenced table always sits
  above the tables referencing it. Tables that nothing connects to are
  packed to one side. Keys that reach outside the schema bring those tables
  in, framed separately.

Database
: Every schema the server does not own itself, each in a frame of its own,
  the frames packed into rows.

Above 150 tables the layout gives up on layers and arranges the tables in a
plain grid — at that size no arrangement helps, and a filtered diagram is
the better answer.

## Moving about

```{index} zooming
```

- {kbd}`Ctrl`+wheel zooms, dragging the background pans.
- Dragging a table moves it; the keys follow.
- *Fit* shows everything, *Actual Size* goes back to 1:1 on the table the
  diagram is about.
- A table's diagram opens big enough to read, even if the rest has to be
  scrolled to; a schema or database diagram opens showing everything.
