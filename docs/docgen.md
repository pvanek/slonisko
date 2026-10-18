# Schema documentation

```{index} documentation, schema documentation, slonisko-docgen, docgen
```

Slonisko writes documentation of a database's schemas: a page for every
table, view, routine and type, with its columns, keys, indexes, triggers and
definition, comments, diagrams of the foreign keys and an index of every
name. It comes in two forms that write the same thing:

- **Generate Schema Documentation…** in the program, in the context menu of
  a database in the browser, on its open connection, SSH tunnel and saved
  password included;
- **`slonisko-docgen`**, a command line tool installed with it, for scripts
  and continuous integration.

The idea comes from [oraschemadoc](https://github.com/pvanek/oraschemadoc),
which did the same for Oracle.

## Formats

```{index} HTML, Markdown, AsciiDoc, Antora, Sphinx, reStructuredText
```

HTML
: A static website: open `index.html`, or put the folder on any web
  server. It needs nothing else, and follows the system's dark mode. SQL is
  highlighted. Diagrams are drawn by [Graphviz](https://graphviz.org) when
  its `dot` program is installed, and each table in them links to its page;
  without Graphviz they are left as their DOT source.

Markdown
: GitHub-flavoured Markdown, for a repository or its wiki. Diagrams are
  [Mermaid](https://mermaid.js.org) blocks, which GitHub and GitLab draw
  themselves.

AsciiDoc
: Plain AsciiDoc for [Asciidoctor](https://asciidoctor.org); the links
  between pages become links between the HTML files it makes. Diagrams are
  `[graphviz]` blocks, drawn by
  [asciidoctor-diagram](https://docs.asciidoctor.org/diagram-extension/latest/)
  or [Kroki](https://kroki.io) and shown as source without either.

AsciiDoc for Antora
: The same pages laid out as an [Antora](https://antora.org) component:
  `antora.yml`, `modules/ROOT/nav.adoc` with every page in it, and the pages
  under `modules/ROOT/pages`. Point a playbook's content source at the
  folder, or commit it to a repository one reads. For the diagrams, add
  the `asciidoctor-kroki` extension to the playbook.

reStructuredText for Sphinx
: A [Sphinx](https://www.sphinx-doc.org) project: `conf.py`, `index.rst`
  and a page per object, the navigation in toctrees. Build it as it is,
  with `sphinx-build -b html . _build/html`, or copy the pages into an
  existing project. Diagrams use Sphinx's own `graphviz` extension, which
  needs Graphviz's `dot` when building; the tables in them link to their
  pages. Links between pages are `:doc:` and `:ref:` cross-references, so
  any builder Sphinx has — HTML, PDF through LaTeX, EPUB — keeps them.

## What is in it

```{index} documentation contents, COMMENT ON
```

Home
: The database, its server version and owner, a table of the schemas with
  how much is in each, and the installed extensions.

A page per schema
: Its diagram, and lists of its tables, views, materialized views, foreign
  tables, functions, procedures, aggregates, types and sequences.

A page per table or view
: Columns with their types, defaults and keys; constraints; foreign keys and
  what references the table; indexes with their sizes; triggers; row level
  security policies; partitions; the views reading it, or what a view reads;
  a diagram of the table and its neighbours; and its definition.

A page per routine and per type
: Arguments, result, language, volatility, the triggers using it and the
  source; an enum's values, a composite's attributes, a domain's checks.

Sequences
: One page per schema with all of them, and the column each one belongs to.

Index of names
: Every table, column, constraint, index, trigger, routine, type and
  sequence, A to Z.

Comments written with `COMMENT ON` are what makes documentation worth
reading: they are shown wherever their object is, the first paragraph in
lists and the whole of it on the object's own page.

Objects that belong to an extension, like PostGIS's functions in `public`,
are left out; the extension is listed on the home page instead. Partitions
are listed on their table's page; *A page for each partition*
(`--partitions`) gives them pages of their own.

A table's definition is put together from the catalog, as the rest is.
It is meant to be read; `pg_dump --schema-only` gives the one to restore
from.

What is worth fixing in the schema, such as a foreign key without an index
or a table without a primary key, is the business of the
{doc}`quality report <quality-report>`, not of the documentation.

## In the program

```{index} Generate Schema Documentation, context menu; database
```

Right-click a database of a connected server in the browser and choose
**Generate Schema Documentation…**. A wizard walks through it:

Schemas
: The database's schemas, all of them ticked; untick what to leave out.

Format and Folder
: What to write and where. Files already in the folder are replaced when
  they have the same names and left alone otherwise; the wizard asks first
  when the folder is not empty. With *Open when finished*, an HTML site
  opens in the browser and other formats show their folder.

Contents
: A title and an introduction for the home page, and what to leave out.
  *Generate* starts the writing.

Writing
: The pages are written in the background, and the wizard does not keep
  the rest of the window waiting. If it fails, *Back* leads to whatever
  needs changing, and *Generate* tries again.

## On the command line

```{index} slonisko-docgen; command line, command line
```

```sh
slonisko-docgen <format> [options] [connection]
```

The format comes first: `html`, `markdown`, `asciidoc`, `antora` or
`sphinx`. Each has the options below and a few of its own;
`slonisko-docgen <format> --help` lists them all. The connection is a libpq connection string or URI, as `psql` takes it.
Without one — or for whatever it leaves out — libpq's `PG*` environment
variables and `~/.pgpass` apply.

```sh
# The HTML site of every schema, into ./shop-schema
slonisko-docgen html "host=db.example.com dbname=shop user=reader"

# Two schemas as an Antora component
slonisko-docgen antora -n sales -n billing -o docs/schema \
    --title "Shop database" --description @docs/intro.txt \
    postgresql://reader@db.example.com/shop
```

| Option | |
|---|---|
| `-o`, `--output` *dir* | Where to write; by default `<database>-schema` in the current directory |
| `-n`, `--schema` *pattern* | Only schemas matching; `*` and `?` are wildcards. May be repeated |
| `-N`, `--exclude-schema` *pattern* | Leave out schemas matching. May be repeated |
| `-t`, `--title` *text* | Title of the documentation; by default the database's name |
| `--description` *text* | Introduction for the home page; `@file` reads it from a file |
| `-W`, `--password` | Ask for the password before connecting |
| `--no-definitions` | Leave out the SQL definitions |
| `--no-diagrams` | Leave out the diagrams |
| `--no-index` | Leave out the index of names |
| `--partitions` | Give partitions pages of their own |
| `-q`, `--quiet` | Print only errors and warnings |

| Format | Its own options | |
|---|---|---|
| `html` | `--dot` *program* | The Graphviz program for the diagrams; by default `dot` on the `PATH` |
| | `--css` *file* | A stylesheet instead of the built-in one |
| `antora` | `--antora-name` *name* | The component's name; `schema` by default |
| | `--antora-version` *version* | The component's version; unversioned by default |

It exits with 0 when the documentation is written, 1 when it cannot be (no
connection, a folder it cannot write to), and 2 for a missing or unknown
format and options it does not understand, such as `--css` for Markdown.

Where it is installed depends on the package: on Linux it is in the
`PATH` (on openSUSE in its own package, `slonisko-docgen`); on macOS it is
`/Applications/Slonisko.app/Contents/MacOS/slonisko-docgen`; on Windows it is
`slonisko-docgen.exe` beside `slonisko.exe`. In the Flatpak it is run as

```sh
flatpak run --command=slonisko-docgen cz.yarpen.slonisko --help
```

where Graphviz is not available, so HTML diagrams stay as source.

The account it connects as only needs to read the catalog, which any
account can: no privileges on the tables themselves are needed.
