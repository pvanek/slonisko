# Slonisko

````{only} qthelp
```{image} images/slonisko.png
:alt: Slonisko
:width: 128px
:align: center
```
````

A fast, keyboard-driven PostgreSQL client built with Qt6.

slonisko talks to the server through raw libpq, fully asynchronously, and
uses PostgreSQL's own parser for statement splitting, error positions and
context-aware completion. It is free software under the GPL, version 3 or
later.

## Why another PostgreSQL client

```{index} motivation, DBeaver, TOra, comparison
```


I have used DBeaver for years, and it is a fine tool. What I never got used
to is how its editor behaves, how much of the machine it wants, and how it
handles transactions — several of those are my own reports in its bug
tracker, filed as *pvanek*.

For the last few years I work with PostgreSQL and nothing else. So rather
than a tool that speaks every database, I wanted one that speaks this one
properly: quick to start, small while it runs, and still able to do a day's
work.

I have written a database client before: [TOra](https://github.com/tora-tool/tora),
an Oracle client that also talks to PostgreSQL and MySQL. Its codebase is
large and PostgreSQL was never what it was built for, so adding what I
wanted there would have been more work than starting from something that is
PostgreSQL-only from the first line.

That is why slonisko exists.

The name is a joke I liked the sound of: *slonisko* is what the Czech
translations of Winnie-the-Pooh call a Heffalump, the imaginary beast Pooh
and Piglet set out to trap. In Czech it is also plainly a big elephant,
which is about as close to PostgreSQL's own mascot as a name can get.

## How it compares

| | Platforms | Licence | Built on | Footprint | Scope |
|---|---|---|---|---|---|
| **slonisko** | Linux, Windows, macOS | GPL-3.0 | C++, Qt Widgets, libpq | small | PostgreSQL only: everyday work, developer first |
| pgAdmin 4 | Linux, Windows, macOS, web | PostgreSQL licence | Python, web UI | large | PostgreSQL, administration first |
| DBeaver CE | Linux, Windows, macOS | Apache-2.0 | Java, Eclipse RCP | large | Many databases, very broad |
| DataGrip | Linux, Windows, macOS | proprietary | Java, IntelliJ | large | Many databases, very broad |
| Beekeeper Studio CE | Linux, Windows, macOS | GPL-3.0 | TypeScript, Electron | medium | Several databases, everyday work |
| TablePlus | macOS, Windows, Linux | proprietary | native | small | Many databases, everyday work |
| psql, pgcli | anywhere with a terminal | PostgreSQL licence, BSD | C, Python | tiny | PostgreSQL, scripting and the shell |

Footprint is a rough impression of start-up time and memory while idle, not
a benchmark; scope is what each tool sets out to do rather than how well it
does it. Corrections are welcome — the point of the table is to say where
slonisko fits, not to score anyone.

```{toctree}
:caption: Using slonisko
:maxdepth: 2

getting-started
connections
editor
results
diagrams
```

```{toctree}
:caption: Reference
:maxdepth: 2

shortcuts
configuration
```

```{toctree}
:caption: Development
:maxdepth: 2

development/building
development/architecture
development/coding-style
```
