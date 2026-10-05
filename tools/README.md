# Packaging

One directory per target, each self-contained:

| Directory   | What is there                                    |
|-------------|--------------------------------------------------|
| `opensuse/` | RPM spec template for the openSUSE Build Service |
| `macos/`    | App bundle and disk image                        |
| `windows/`  | Portable zip and Inno Setup installer            |
| `flatpak/`  | Flatpak manifest and a bundle of it              |

Whatever more than one of them needs belongs in the source tree where the
build installs it, not copied here: the desktop file and the AppStream
metainfo, both named after the application ID `cz.yarpen.slonisko`, are in
`src/app`.

The version is kept in one place, `project()` in `CMakeLists.txt`, with the
release date beside it. Everything else is made from there: the metainfo
and the RPM spec from their `.in` templates, the program's own version, the
macOS bundle's, the package names and the manual's. A release changes those
two lines and nothing else.

## openSUSE

The spec is generated from `opensuse/slonisko.spec.in`, which has no version
of its own, by a CMake script that needs nothing else from the build:

```sh
cmake -P tools/opensuse/make-spec.cmake     # build-opensuse/slonisko.spec
```

It builds offline, as OBS requires: libpg_query is not
packaged for openSUSE, so its tarball is a second source and CMake is pointed
at it instead of downloading. Its version in the spec follows the one pinned
in `cmake/SloniskoPgQuery.cmake`. QScintilla, QtKeychain and libpq come from
the distribution. The manual is built into the Qt help file;
`--without docs` leaves it out.

To try it locally:

```sh
version=$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -n 1)
mkdir -p ~/rpmbuild/SOURCES
git archive --prefix=slonisko-$version/ -o ~/rpmbuild/SOURCES/slonisko-$version.tar.gz HEAD
curl -L -o ~/rpmbuild/SOURCES/libpg_query-18.0.0.tar.gz \
    https://github.com/pganalyze/libpg_query/archive/refs/tags/18.0.0.tar.gz
cmake -P tools/opensuse/make-spec.cmake
rpmbuild -bb build-opensuse/slonisko.spec
```

On OBS, upload the same two tarballs with the generated spec, or let the
`download_files` service fetch them from the `Source` URLs.

## What goes into the macOS and Windows packages

Neither system has the libraries, so the package carries them all:

| Library      | How it gets in                                               |
|--------------|--------------------------------------------------------------|
| libpg_query  | Built from its pinned tarball and linked statically          |
| QScintilla   | The same                                                     |
| Qt           | `macdeployqt` / `windeployqt`, plugins included              |
| libpq        | Copied with everything it loads (OpenSSL, Kerberos, zlib...) |
| QtKeychain   | Copied like libpq on macOS; built statically on Windows      |

Each script fails rather than produce a package that still reaches for a
library on the build machine. The manual goes in as the Qt help file:
`Contents/Resources` in the bundle, `share/slonisko` on Windows.

Tests are not run by these scripts; run `ctest` in a normal build first.

## macOS

Needs Xcode's command line tools and [Homebrew](https://brew.sh); the
script installs the rest (Qt, libpq, QtKeychain, CMake, Ninja) itself, and
Sphinx into a virtualenv for the manual.

```sh
tools/macos/build.sh        # build-macos/Slonisko-<version>.dmg
```

Without more, the bundle is signed ad hoc: it runs on the Mac that built
it, and anywhere else only after the user clears the quarantine. To ship it,
sign with a Developer ID and have Apple notarise it:

```sh
xcrun notarytool store-credentials slonisko-notary   # once
SLONISKO_SIGN_IDENTITY="Developer ID Application: Name (TEAMID)" \
SLONISKO_NOTARY_PROFILE=slonisko-notary tools/macos/build.sh
```

Homebrew builds for the macOS it runs on, and so does this: the package
needs that version or later, and the processor of the build machine
(Apple silicon or Intel). `SLONISKO_WITH_DOCS=OFF` leaves the manual out.

## Windows

Needs, installed by hand:

- Visual Studio 2022 (or its Build Tools) with the C++ workload, which
  brings CMake and Ninja
- Qt 6.5 or later for MSVC 64-bit, from the Qt installer or
  `aqt install-qt windows desktop 6.8.3 win64_msvc2022_64`
- PostgreSQL 17 or later from the
  [EDB installer](https://www.enterprisedb.com/downloads/postgres-postgresql-downloads)
  (the "Command Line Tools" component is enough) or its binaries zip
- PowerShell 7.3 or later, Python 3 for the manual, Git for Windows' `tar`
  or Windows' own
- Optional: [Inno Setup 6](https://jrsoftware.org/isinfo.php) for the
  installer

```powershell
tools\windows\build.ps1 -QtDir C:\Qt\6.8.3\msvc2022_64
```

It enters the Visual Studio environment itself, builds QtKeychain (pinned
in the script) as a static library once into `build-windows\deps`, then Slonisko. The program
is assembled in `build-windows\Slonisko`, Qt DLLs by `windeployqt` and the
rest by `deploy-dlls.cmake`, which asks CMake what the program and its
plugins load and copies whatever is not part of Windows; the C++ runtime
goes along so nothing needs installing first. Out come
`Slonisko-<version>-win64.zip` and, with Inno Setup,
`Slonisko-<version>-setup.exe`, which installs per user without
administrator rights or for everyone.

`-PgDir` picks the PostgreSQL installation (default: the newest one in
Program Files), `-NoDocs` leaves the manual out, and `-SignCertificate`
signs the program and the installer with `signtool`.

SSH tunnels use the OpenSSH client that Windows 10 and later include.

## Flatpak

Needs `flatpak` with the Flathub remote; the script installs
`flatpak-builder` (as `org.flatpak.Builder`) and the KDE 6.11 SDK itself.

```sh
tools/flatpak/build.sh              # build-flatpak/Slonisko-<version>.flatpak
tools/flatpak/build.sh --install    # and install it for this user
flatpak run cz.yarpen.slonisko
```

The KDE runtime brings Qt, Qt Help, OpenSSL, libsecret and the `ssh`
client. The manifest builds libpq (from the PostgreSQL release tarball,
client library only) and QtKeychain; libpg_query and QScintilla are built
into the program, as on macOS and Windows. Flatpak builds have no network,
so every source is listed in the manifest with its hash. The manual is
built into the help file too, with Sphinx from `python3-docs.yaml`.

The sandbox gets the network, the home directory (scripts, `~/.pgpass`,
`~/.ssh`), the SSH agent and the system wallet. Settings and saved
connections live in `~/.var/app/cz.yarpen.slonisko`, apart from those of a
slonisko installed any other way.

When a pinned version changes, change the manifest with it: libpg_query and
QScintilla follow `cmake/`. `python3-docs.yaml` is generated from
`docs-requirements.txt` with
[flatpak-pip-generator](https://github.com/flatpak/flatpak-builder-tools/tree/master/pip):

```sh
cd tools/flatpak
flatpak-pip-generator --runtime org.kde.Sdk//6.11 --build-only --yaml \
    -r docs-requirements.txt -o python3-docs
```

For Flathub, the manifest's first source becomes the release tag in git
instead of this checkout, and the metainfo wants screenshots.

## Releases

`.github/workflows/release.yml` runs these scripts on GitHub for every tag
`v<version>`: macOS on Apple silicon and on Intel, Windows, and the Flatpak.
The tag has to match `VERSION` in `CMakeLists.txt`, or nothing is built.
The packages, with a `SHA256SUMS` of them, go into a draft release; its
notes are written by hand before it is published, as they are the changelog
the manual points to. Started by hand instead (*Run workflow*), it builds
the packages from any branch and keeps them as the run's artifacts, without
a release.

```sh
version=$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -n 1)
git tag -a "v$version" -m "Slonisko $version"
git push origin "v$version"
```

Without further setup the macOS disk images are signed ad hoc, and the
Windows program is not signed. For a Developer ID signature and
notarisation, set these repository secrets:

| Secret                       | What it holds                                      |
|------------------------------|----------------------------------------------------|
| `MACOS_CERTIFICATE`          | The Developer ID Application certificate, `.p12`, base64 |
| `MACOS_CERTIFICATE_PASSWORD` | The `.p12`'s password                              |
| `MACOS_SIGN_IDENTITY`        | `Developer ID Application: Name (TEAMID)`          |
| `MACOS_NOTARY_APPLE_ID`      | The Apple ID notarisation runs as                  |
| `MACOS_NOTARY_PASSWORD`      | An app-specific password of that Apple ID          |
| `MACOS_NOTARY_TEAM_ID`       | The team ID                                        |
