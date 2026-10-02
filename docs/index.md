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
uses PostgreSQL's own parser for context-aware completion and
highlighting. It is free software under the GPL, version 3 or later.

```{figure} images/main-window.png
:alt: The main window: the object browser, a SQL editor and its results
```

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

## Changelog

```{index} changelog, releases, version history
```

What changed in each version is written up with its release on GitHub:
[github.com/pvanek/slonisko/releases](https://github.com/pvanek/slonisko/releases).
Help → *About Slonisko* tells which version is running. For changes not
released yet, see the
[commit history](https://github.com/pvanek/slonisko/commits/main).

## Author

```{index} author, contact, support, donation
```

slonisko is written by Petr Vanek, in Czechia. I have been writing and
maintaining open source software for a long time, mostly in C++ and Qt:
[TOra](https://github.com/tora-tool/tora), [Scribus](https://scribus.net),
Razor-qt and a few smaller tools along the way. More about me is on
[yarpen.cz](https://yarpen.cz).

Bugs and ideas are best reported as
[issues on GitHub](https://github.com/pvanek/slonisko/issues), where others
can find them too. Anything else goes to
[petr@yarpen.cz](mailto:petr@yarpen.cz).

### Supporting the work

```{index} donation, supporting slonisko
```

I write slonisko primarily for myself, and it is free and will stay so.
Doing it properly — packages for several platforms, testing, downloads —
still takes time and some money. If slonisko is useful to you and you would
like to help, [Buy me a coffee](https://buymeacoffee.com/pvanek) or visit the
[open source page on yarpen.cz](https://yarpen.cz/en:opensource) to see how
to send a small donation. Thank you.

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
