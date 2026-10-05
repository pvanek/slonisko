# Building

```{index} building, dependencies
```

## What is needed

- CMake 3.25 and a C++20 compiler
- Qt 6.5 or later: Core, Gui, Widgets, Network, Concurrent, Test
- libpq 17 or later (the client library; any supported server works)
- [libpg_query](https://github.com/pganalyze/libpg_query)
- [QScintilla](https://riverbankcomputing.com/software/qscintilla) for Qt 6
- Optional: [QtKeychain](https://github.com/frankosterfeld/qtkeychain) for
  the system wallet, Qt's Help module to show this manual in the program,
  the OpenSSH client for tunnels, Docker for the database tests
- Sphinx with the MyST parser for the manual, which is on by default;
  without it, configure with `-DSLONISKO_WITH_DOCS=OFF`

On openSUSE Tumbleweed:

```sh
sudo zypper install cmake gcc-c++ qt6-base-devel postgresql17-devel \
    qtkeychain-qt6-devel qscintilla-qt6-devel
```

libpg_query is usually not packaged. Either build it and point CMake at it
with `-DPgQuery_ROOT=/path/to/libpg_query`, or let CMake fetch and build a
pinned copy with `-DSLONISKO_BUNDLED_PGQUERY=ON`. QScintilla works the same
way: the system copy is used when it is there, and
`-DSLONISKO_BUNDLED_QSCINTILLA=ON` builds a pinned one, which is what
Windows and macOS do.

## Building and testing

```{index} tests, Docker
```

```sh
cmake -S . -B build -DSLONISKO_BUNDLED_PGQUERY=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/src/app/slonisko
```

The database tests start a throwaway `postgres:18` container with Docker and
remove it afterwards. Without Docker they are skipped; to use a server you
already have, set `SLONISKO_TEST_CONNINFO` to a libpq connection string. The
tests create schemas, tables and a database of their own and drop them
again, so use a server where that is harmless.

## Building this manual

```{index} building the documentation, Sphinx, help file
```

The manual is built twice from the same Markdown sources: into a website,
and into the Qt help file the program shows itself.

```sh
sudo zypper install python313-Sphinx python313-myst-parser python313-furo
cmake -S . -B build -DSLONISKO_WITH_DOCS=ON
cmake --build build --target docs           # both of the below
cmake --build build --target docs-html      # build/docs/html
cmake --build build --target docs-qch       # build/docs/slonisko.qch
```

`docs-qch` needs `qhelpgenerator`, which comes with Qt's tools, and copies
the help file next to the program so a build directory run finds it (into
`Contents/Resources` of a macOS bundle); `cmake --install` puts it in
`share/slonisko`. For the program to show it,
Qt's Help module has to be there when the program is built
(`qt6-help-devel` on openSUSE); without it the Help menu opens the
project's page on GitHub instead.

Sphinx can also be run directly, once the icon is in place (see below):

```sh
cp src/app/icons/slonisko-128.png docs/images/slonisko.png
sphinx-build -b html docs build/docs/html
```

The website is published on GitHub Pages, at
<https://pvanek.github.io/slonisko/>, by `.github/workflows/website.yml`:
whenever the manual changes on `main`, it is built the same way, with
warnings treated as errors, and deployed. For that, the repository's Pages
source has to be set to *GitHub Actions*, once.

One generated file is involved: the manual shows the program's icon, and
Sphinx takes images only from its own directory, so `docs/images/slonisko.png`
is copied there from `src/app/icons`. The documentation build does that
itself, and `src/app/icons/generate.sh` writes it along with the other
sizes; the file is not in the repository.

## Screenshots

```{index} screenshots
```

The pictures in the manual are taken by the program itself, not by hand:
`tools/screenshots` drives the real main window against a small sample
database and saves each page into `docs/images`. After a change to how
something looks, take them again:

```sh
cmake -S . -B build -DSLONISKO_SCREENSHOTS=ON
tools/screenshots/run.sh build
```

It needs Docker, for a throwaway `postgres:18` loaded with
`tools/screenshots/shop.sql`, and `Xvfb`, so the windows open on a display
of their own. The settings are kept in a temporary directory: your own
connections are neither shown nor touched.

## Packages

```{index} packaging, RPM, openSUSE Build Service, macOS, Windows, Flatpak
```

Packaging lives in `tools/`, one directory per target; `tools/README.md`
says what is there and how to build each. The openSUSE spec builds without
network access, as the Build Service requires, by shipping libpg_query's
tarball as a second source. On macOS and Windows a script builds the package
with every library inside it: `tools/macos/build.sh` makes a disk image,
`tools/windows/build.ps1` a zip and an installer, and
`tools/flatpak/build.sh` a Flatpak bundle.
