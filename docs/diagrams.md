# Diagrams

```{index} diagrams, ER diagram
```

Tables, schemas and databases have a *Diagram* tab showing their foreign
keys. The diagram is drawn by the program itself — there is no Graphviz or
other outside tool involved — and it is loaded only when the tab is opened.

```{figure} images/diagram.png
:alt: The diagram of a schema with six tables connected by foreign keys

A schema's diagram: referenced tables above the tables that reference them.
```

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
  above the tables referencing it. Groups of tables that no key connects
  are laid out on their own, side by side. Keys that reach outside the
  schema bring those tables in, framed separately.

Database
: Every schema the server does not own itself, each in a frame of its own,
  the frames packed into rows.

Above 150 tables the layout gives up on layers and arranges the tables in a
plain grid — at that size no arrangement helps, and a filtered diagram is
the better answer.

## Moving about

```{index} zooming
```

- {kbd}`Ctrl`+wheel or *Zoom In* and *Zoom Out* zoom, dragging the
  background pans. Zoomed far out, the boxes show only the tables' names.
- Dragging a table moves it; the keys follow.
- *Fit* shows everything, *Actual Size* goes back to 1:1 on the table the
  diagram is about.
- A table's diagram opens big enough to read, even if the rest has to be
  scrolled to; a schema or database diagram opens showing everything.

## Saving a diagram

```{index} export; diagram, SVG, PNG, PDF, Graphviz, Mermaid
```

*Export* on the diagram's toolbar opens a menu. *Copy as Mermaid* and
*Copy as Graphviz* put the diagram's source on the clipboard, ready to paste
into a wiki page or a README. The *Save as…* entries ask for a file, in one
of five formats:

SVG, PNG, PDF
: The diagram as it is drawn, including tables you have moved, on a white
  background whatever colours the program uses. SVG and PDF keep the text as
  text, so it can be searched and stays sharp; a PNG is drawn at twice the
  screen size, or less if the diagram is very large.

Graphviz (`.dot`, `.gv`)
: Source for Graphviz's `dot`, which lays the tables out itself, for
  example `dot -Tsvg schema.dot -o schema.svg`. Each schema gets a frame
  when there is more than one.

Mermaid (`.mmd`)
: An `erDiagram` for Mermaid, which GitHub, GitLab and many wikis draw in
  Markdown. Mermaid accepts fewer characters in names and types than
  PostgreSQL; anything it would refuse is replaced by `_`, and the original
  spelling is kept as the column's comment.

The text formats list every key column; the picture shows at most eighteen
per table.
