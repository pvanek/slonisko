# The object browser

```{index} object browser, connection tree, database objects
```

The connection tab on the left is the object browser: the saved
connections, sorted into folders, and under each connected one what the
server holds. This page lists what it shows and what each object's page
says about it.

The tree reads the catalog when a node is first expanded, and keeps nothing
between sessions. {kbd}`F5` or *Refresh* reads the selected node again, with
everything under it; objects created or dropped meanwhile are not noticed
before that.

## What is in the tree

```{index} databases, schemas, Show all databases
```

Under a connection come its databases, then *DBA Tools* and *System Info*:

```text
Connection
├── Database
│   ├── Schemas
│   │   └── Schema
│   │       ├── Tables              ── Columns, Constraints, Indexes, Triggers, Policies
│   │       │                          (and Partitions, for a partitioned table)
│   │       ├── Views               ── Columns, Triggers
│   │       ├── Materialized Views  ── Columns, Indexes
│   │       ├── Foreign Tables      ── Columns, Constraints, Triggers
│   │       ├── Sequences
│   │       ├── Functions
│   │       ├── Procedures
│   │       ├── Aggregates
│   │       ├── Types
│   │       └── Domains
│   ├── Extensions
│   ├── Foreign Data Wrappers
│   │   └── Foreign Data Wrapper
│   │       └── Foreign Servers ── User Mappings
│   ├── Publications
│   └── Event Triggers
├── DBA Tools
│   ├── Sessions, Locks, …
│   ├── Roles
│   └── Tablespaces
└── System Info
    └── Server Overview, Changed Settings, …
```

A connection shows the one database it was made to, unless *Show all
databases of the server* is ticked in its settings; then it lists every
database that accepts connections, leaving out the templates. Each further
database the tree opens gets a connection of its own, see
{doc}`connections`.

Objects that come with PostgreSQL — the `pg_catalog` and
`information_schema` schemas, the `pg_` roles and tablespaces — are greyed
out. TOAST and temporary schemas are not listed.

### Server-wide

[Databases](https://www.postgresql.org/docs/current/managing-databases.html)
: Those that allow connections and are not templates, when *Show all
  databases* is on.

[Roles](https://www.postgresql.org/docs/current/user-manag.html)
: Under *DBA Tools*, marked as superuser or able to log in.

[Tablespaces](https://www.postgresql.org/docs/current/manage-ag-tablespaces.html)
: Under *DBA Tools*, with their location on the server.

### In a database

[Schemas](https://www.postgresql.org/docs/current/ddl-schemas.html)
: The system ones last.

[Extensions](https://www.postgresql.org/docs/current/extend-extensions.html)
: With the installed version.

[Foreign Data Wrappers](https://www.postgresql.org/docs/current/ddl-foreign-data.html)
: Each holds its [foreign servers](https://www.postgresql.org/docs/current/sql-createserver.html), and each
  server its [user mappings](https://www.postgresql.org/docs/current/sql-createusermapping.html), named after
  the role they are for or `public`. A mapping's password is shown as
  `********`; its other options only to those PostgreSQL lets see them.

[Publications](https://www.postgresql.org/docs/current/logical-replication-publication.html)
: Tables whose changes this database sends to logical replication
  subscribers. Subscriptions are not in the tree; *Replication* under
  *System Info* shows the connected ones.

[Event Triggers](https://www.postgresql.org/docs/current/event-triggers.html)
: With the event they fire on.

### In a schema

[Tables](https://www.postgresql.org/docs/current/ddl-basics.html)
: Ordinary and partitioned ones. Partitions are listed under their table,
  not here.

[Views](https://www.postgresql.org/docs/current/sql-createview.html), [Materialized Views](https://www.postgresql.org/docs/current/rules-materializedviews.html)

[Foreign Tables](https://www.postgresql.org/docs/current/ddl-foreign-data.html)
: Those that are not partitions.

[Sequences](https://www.postgresql.org/docs/current/sql-createsequence.html)

[Functions](https://www.postgresql.org/docs/current/xfunc.html)
: Ordinary and window functions, with arguments and result type. Those
  made along with another object, like a range type's constructors, are
  left out.

[Procedures](https://www.postgresql.org/docs/current/xproc.html)

[Aggregates](https://www.postgresql.org/docs/current/xaggr.html)
: With arguments and result type.

[Types](https://www.postgresql.org/docs/current/sql-createtype.html)
: Base, composite, enum, range and pseudo-types. The array and row types
  PostgreSQL makes on its own are left out.

[Domains](https://www.postgresql.org/docs/current/domains.html)
: With their base type.

### In a table, view or similar

Columns
: With their type, and `not null` where it applies.

[Constraints](https://www.postgresql.org/docs/current/ddl-constraints.html)
: Except `NOT NULL`, which shows on the columns.

[Indexes](https://www.postgresql.org/docs/current/indexes.html)
: Marked primary key or unique.

[Triggers](https://www.postgresql.org/docs/current/triggers.html)
: Without the internal ones behind foreign keys.

[Policies](https://www.postgresql.org/docs/current/ddl-rowsecurity.html)
: Row-level security policies, with their command.

[Partitions](https://www.postgresql.org/docs/current/ddl-partitioning.html)
: On a partitioned table, with their bounds.

### Not shown

The tree has nothing yet for collations, conversions, operators and
operator classes, casts, languages, text search configurations,
subscriptions, rules and extended statistics. The editor can still query them, of course, and
{doc}`docgen` and the {doc}`quality-report` read the catalog on their own.

## An object's page

```{index} object details, object page, Definition tab
```

Double-clicking an object, or *Details* in its context menu, opens a page
about it; for a database, which expands on double-click, *Details* is the
way. {kbd}`Ctrl`+click on a name in the editor opens the same page, see
[Opening what a name is](editor.md#opening-what-a-name-is).

Every page starts with an *Overview* of the object's properties — owner,
size, comment and whatever else applies — leaving out those that are not
set. The tabs after it depend on the kind:

| Object | Tabs after the overview |
|---|---|
| Database | *Settings* set per database or per role in it, when there are any; *Diagram* |
| Schema | *Contents*: how many objects of each kind it holds; *Dependencies*; *Diagram* |
| Table, partitioned table, foreign table | *Columns*, *Indexes*, *Constraints*, *Triggers*, *Policies* when there are any; *Data*; *Dependencies*; *Diagram* |
| View, materialized view | *Columns*, *Indexes* when there are any; *Data*; *Definition*; *Dependencies* |
| Sequence | *Dependencies*; the overview has its range, increment, last value and the column that owns it |
| Function, procedure | *Definition*, the `CREATE` statement; *Dependencies* |
| Aggregate | *Support*: its transition and final functions and state type; *Dependencies* |
| Type, domain | *Values* of an enum, *Attributes* of a composite type, *Constraints* of a domain, as apply; *Dependencies* |
| Index | *Columns*, with expressions; *Definition*; *Dependencies* |
| Trigger | *Definition* |
| Constraint | *Dependencies*; the overview has the definition |
| Policy | — the overview has the definition |
| Extension | *Objects* the extension owns; *Dependencies* |
| Foreign data wrapper | *Servers*, when there are any |
| Foreign server | *User Mappings* and *Foreign Tables* using it, when there are any |
| User mapping | — the overview has the user, the server and the options |
| Publication | *Tables* it publishes |
| Event trigger | — |
| Role | *Member of* and *Members*, when there are any |
| Tablespace | *Databases* kept in it, when there are any |

A column has no page of its own; its table's *Columns* tab says all there
is. The *Diagram* tab is described in {doc}`diagrams`, the *Dependencies*
tab below.

## A look at the rows

```{index} data preview, Data tab
```

The *Data* tab of a table, view, materialized view or foreign table shows
its first 1000 rows, `SELECT * FROM` it with a `LIMIT`, in no particular
order. The query runs only once the tab is opened, since a view or a foreign
table may take a while, and again on *Refresh* or *Run Again*; *Stop*
cancels it. When the limit is reached, a note above the rows says so, so
that the row count is not taken for the table's size.

The rows are read-only here. Clicking a column's header sorts the rows that
were fetched, not the whole table. They can be copied, exported and viewed
as text or one record at a time, as in the editor's results, see
{doc}`results`. To edit them, or to see more of them, query the table in an
editor.

## What depends on an object

```{index} dependencies, impact of a change, DROP CASCADE, ALTER COLUMN TYPE, pg_depend
```

Before dropping an object or changing a column's type, the *Dependencies*
tab shows what that would hit: everything that depends on the object, and
everything that depends on those in turn, as a tree. *Dependencies* in an
object's context menu opens its page at that tab. Its queries run when the
tab is first opened, since they read the dependencies of the whole
database; on a big one that takes a moment.

Each row says what the object is, how it depends on the one above it — a
view's query uses it, a foreign key references it, a trigger executes it, a
policy uses it, a column is of this type — and on which of its columns, when
that is known. Double-clicking a row opens that object's page.

The last column says what a `DROP` would do to it:

needs CASCADE, then dropped
: A plain `DROP` is refused because of it; `DROP … CASCADE` drops it too.
  Views, foreign keys from other tables, policies and functions in standard
  SQL (`BEGIN ATOMIC`, `RETURN`) are of this kind.

dropped with it
: Goes along without asking: the object's own indexes, constraints,
  triggers and policies, its partitions, the sequences it owns.

stays, fails when run
: PostgreSQL does not know about it, so neither refuses the `DROP` nor
  drops it, and it fails the next time it runs. Shown in italics. These
  are functions whose body is a string — PL/pgSQL, and SQL written with
  `AS '…'` — that name the object, and whatever uses those functions.

The line above the tree adds it up: how many objects would refuse a plain
`DROP`, how many `CASCADE` would take along, how many would be left to fail.

### Function bodies

The server records nothing about what a PL/pgSQL body or an SQL body in a
string refers to: it checks only when the function runs. So Slonisko reads
the bodies itself, with the same parser as the editor, and looks up the
tables, views, sequences (in `nextval('…')` and the like), functions and
types they name the way the server would: in the function's own
`search_path` if it sets one, else in the connection's, and failing that in
the function's own schema. That is a good guess rather than a certainty:

- what `EXECUTE` runs is only known when it runs, so it is not looked at;
- the type of a PL/pgSQL variable is not read, except for `%ROWTYPE` and
  `%TYPE`, which name a table;
- a body the parser cannot read — it gives up on a PL/pgSQL variable whose
  type is in a schema other than `public`, for one — is searched for the
  names after `FROM`, `JOIN`, `INTO` and `UPDATE` and for what is called
  like a function, and the summary lists those functions.

Functions that belong to an extension are left out.

### One column

For a table, view or similar, *What depends on* can be set to one of its
columns instead of the whole object, to see what `ALTER COLUMN … TYPE`
would hit: views that use the column, indexes and constraints on it,
foreign keys referencing it, triggers that fire on its update, functions
whose body names the table and mentions a column of that name or `*`. A
view on the column makes the server refuse the change until the view is
dropped; indexes and constraints are rebuilt. Past the first level whole
objects count, so a view on such a view is listed even when it does not
use that column.

## What else the context menu offers

```{index} context menu, object browser
```

Besides *Details* and *Refresh*, the right-click menu has what applies to
the node:

- on an object that others can depend on: *Dependencies*, see
  [What depends on an object](#what-depends-on-an-object);
- on a connection or database: *Open SQL Editor*, *Back Up…* and
  *Restore…* (see {doc}`import-and-backup`); on a database also *Generate
  Schema Documentation…* ({doc}`docgen`) and *Database Quality Report…*
  ({doc}`quality-report`);
- on a schema: *Back Up Schema…* and *Import CSV into a New Table…*;
- on a table: *Back Up Table…* and *Import CSV…*;
- anywhere under a connection: *Reconnect* and *Disconnect*, since what
  hangs is usually a folder deep in the tree that keeps loading.

The items under *DBA Tools* and *System Info* are prepared queries rather
than objects; opening one is described in
[DBA Tools and System Info](results.md#dba-tools-and-system-info).
